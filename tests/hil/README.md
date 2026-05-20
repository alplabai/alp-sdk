# Hardware-in-loop (HiL) smoke tests

Real-silicon verification harness.  The layout is two-tier:

| Directory                          | Role                                                |
|------------------------------------|-----------------------------------------------------|
| [`_common/`](_common/)             | Portable smoke specs that work on every E1M SoM.  One per peripheral example under `examples/`. |
| `<sku>-<board>/`                 | Per-board directory.  Carries a `_runner.yaml` (board target, serial port, flash method) and ANY SoM-specific specs that don't apply to other SoMs. |

A runner script ([`run_smoke.py`](run_smoke.py)) reads the specs
and drives the hardware.  When invoked against a board directory
(e.g. `tests/hil/aen701-evk/`), it walks `_common/` + the board's
own specs, using the board's `_runner.yaml` for every spec.  A
board-local spec named identically to a shared one **overrides**
it (per-board tuning).

This complements (not replaces) `tests/zephyr/` and `tests/scripts/`:

| Surface              | Where               | What it proves                          |
|----------------------|---------------------|------------------------------------------|
| `tests/scripts/`     | Pure host           | The loader / validator / schemas behave |
| `tests/zephyr/`      | `native_sim/native/64` ZTEST | The SDK's failure-paths are sensible (NULL args, NOSUPPORT, etc.) |
| `tests/hil/`         | **Real silicon**    | The SDK's *happy* paths actually drive the I/O the wrapper claims they drive |

A row in `docs/test-plan.md` flips from `⏳` to `✅` only after a
HiL spec under this tree passes against the matching board.

---

## What you need

| Item          | Spec                                                        |
|---------------|-------------------------------------------------------------|
| **SoM**       | One supported per smoke-spec directory (today: AEN701)      |
| **Board**   | The E1M EVK that matches (today: E1M-EVK -- 35x35 reference)|
| **Debug**     | SEGGER J-Link or Alif's recommended SWD adapter on J2       |
| **Serial**    | USB-C from the EVK exposes the UART (Linux: `/dev/ttyACM0`) |
| **Power**     | 12 V barrel jack OR USB-C from the runner host              |
| **Toolchain** | Zephyr workspace at `$ZEPHYR_BASE` + the per-SoM board file |

The per-SoM Zephyr board target (e.g. `alp_e1m_evk_aen` for AEN701)
ships in [`alplabai/alp-zephyr-modules`](https://github.com/alplabai/alp-zephyr-modules)
once published.  Until that lands, point the runner at your own DT
overlay via `--board` and `--west-args` (the runner is target-agnostic).

---

## Quick start

Wire the board (J-Link on J2, USB-C on the host), then from the SDK
root:

```bash
# Lint every spec for AEN701 (12 from _common/, 0 board-specific).
# No hardware needed.
python3 tests/hil/run_smoke.py --validate tests/hil/aen701-evk/

# Dry-run -- print every `west build` / `west flash` command without
# running them.  Useful to confirm board targets + paths.
python3 tests/hil/run_smoke.py --dry-run tests/hil/aen701-evk/

# Real run.  Builds, flashes, captures serial, asserts.
python3 tests/hil/run_smoke.py tests/hil/aen701-evk/

# V2N101 mode -- pulls in 12 portable specs + 2 V2N-specific ones
# (GD32 bridge ping, on-module TMP112).
python3 tests/hil/run_smoke.py tests/hil/v2n101-x-evk/

# Suppress the portable set -- run ONLY board-specific specs.
python3 tests/hil/run_smoke.py --no-common tests/hil/v2n101-x-evk/

# One spec at a time:
python3 tests/hil/run_smoke.py tests/hil/_common/gpio-button-led.yaml
```

The runner exits 0 if every spec passed, 1 if any failed, 2 on
invocation error (missing spec dir, malformed YAML, etc.).

## Currently supported boards

Each entry below ships a `_runner.yaml` with a board target.  The
Zephyr board target itself lives in [`alplabai/alp-zephyr-modules`](https://github.com/alplabai/alp-zephyr-modules)
(per the SoM preset's `topology.<core>.board` field).  Until that
module publishes, override per-invocation with `--board` or by
editing the dir's `_runner.yaml`.

| Board dir              | SoM          | Board       | Board target                  |
|------------------------|--------------|---------------|-------------------------------|
| `aen301-evk/`          | E1M-AEN301   | E1M-EVK       | `alp_e1m_aen301_m55_hp`       |
| `aen401-evk/`          | E1M-AEN401   | E1M-EVK       | `alp_e1m_aen401_m55_hp`       |
| `aen501-evk/`          | E1M-AEN501   | E1M-EVK       | `alp_e1m_aen501_m55_hp`       |
| `aen601-evk/`          | E1M-AEN601   | E1M-EVK       | `alp_e1m_aen601_m55_hp`       |
| `aen701-evk/`          | E1M-AEN701   | E1M-EVK       | `alp_e1m_aen701_m55_hp`       |
| `aen801-evk/`          | E1M-AEN801   | E1M-EVK       | `alp_e1m_aen801_m55_hp`       |
| `v2n101-x-evk/`        | E1M-V2N101   | E1M-X-EVK     | `alp_e1m_v2n101_m33_sm`       |
| `v2n102-x-evk/`        | E1M-V2N102   | E1M-X-EVK     | `alp_e1m_v2n102_m33_sm`       |
| `v2m101-x-evk/`        | E1M-V2M101   | E1M-X-EVK     | `alp_e1m_v2m101_m33_sm`       |
| `v2m102-x-evk/`        | E1M-V2M102   | E1M-X-EVK     | `alp_e1m_v2m102_m33_sm`       |
| `nx9101-evk/`          | E1M-NX9101   | E1M-EVK       | `alp_e1m_nx9101_m33`          |

---

## Spec format

Each `*.yaml` file under `<sku>-<board>/` describes one smoke test:

```yaml
# tests/hil/aen701-evk/gpio-button-led.yaml
schema_version: 1
name: gpio-button-led
description: Verifies GPIO read (button) + GPIO write (LED) end-to-end.

# The example to build and flash.  Paths are repo-relative.
example: examples/gpio-button-led

# Zephyr board target.  When omitted, defaults to the per-directory
# default declared in <dir>/_runner.yaml.
board: alp_e1m_evk_aen

# Serial assertion: capture `duration_s` seconds of output after flash;
# the test passes iff every `expect_contains` string is present (case-
# insensitive) and no `expect_absent` string appears.
serial:
  duration_s: 15
  baud:       115200
  expect_contains:
    - "ALP SDK"
    - "alp_gpio_open ok"
    - "button=PRESSED"
    - "led=ON"
  expect_absent:
    - "ALP_ERR_"             # any error code in the log fails the test
    - "PANIC"
    - "ASSERT"
```

Per-directory defaults live in `<sku>-<board>/_runner.yaml`:

```yaml
schema_version: 1
board:    alp_e1m_evk_aen
serial_port:  /dev/ttyACM0
flash_method: westflash         # or pyocd-flash
defaults:
  serial:
    baud:       115200
    duration_s: 15
```

The runner merges per-spec values on top of these.

---

## What this tree does NOT do

- **No CI runner installation.**  The runner lives at
  `/opt/alp-hil/` on the self-hosted runner host; setup is documented
  in [`docs/ci/HW-IN-LOOP.md`](../../docs/ci/HW-IN-LOOP.md).
- **No fixture model.**  Specs that need external probes (logic
  analyser for I²S tone capture, USB sound card for audio loopback,
  external relay for power cycling) are tracked under future
  `*-fixture.yaml` spec files; the v0 scope is what a bare EVK can
  exercise via serial output alone.
- **No retry on failure.**  Flakes get an explicit `tags: [flaky]`
  in the spec and the runner skips them with a SKIP marker; the
  expectation is to investigate and unflake within the release cycle
  (matches the quarantine policy in `docs/ci/HW-IN-LOOP.md`).

---

## Adding a new SoM

1. Create `tests/hil/<sku>-<board>/_runner.yaml` with the
   per-directory defaults (board target, serial port, flash method).
   The 12 portable specs in `_common/` apply automatically.
2. Add SoM-specific YAML specs alongside `_runner.yaml` for the
   peripherals only that SoM has (e.g. V2N's GD32 supervisor bridge,
   V2M's DEEPX NPU bring-up, AEN's CC3501E Wi-Fi).
3. Run `python tests/hil/run_smoke.py --validate tests/hil/<sku>-<board>/`
   to confirm every spec parses against the board target.
4. Add the runner label (`hil-<sku>`) to the matching
   `.github/workflows/nightly-<sku>-hil.yml`.
5. Once a HiL run passes, flip the relevant rows in
   `docs/test-plan.md` from `⏳` to `✅`.

## Adding a new portable peripheral

When `examples/<peripheral-name>/` lands a new peripheral demo that
works on every E1M SoM:

1. Drop `tests/hil/_common/<peripheral-name>.yaml` with the
   `expect_contains` strings sourced from the example's printf
   output.
2. The test `test_shipped_common_dir_has_one_spec_per_peripheral`
   in `tests/scripts/test_hil_run_smoke.py` enforces that every
   required peripheral has a spec; add the entry there too.
3. The spec automatically applies to every board dir on the next
   run.
