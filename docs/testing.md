# Testing the ALP SDK

How to verify everything works, from a fresh clone, without touching
hardware.  Coverage map per `<alp/...>` header below.

> **What this doc is.**  An index of *which tests exist for which
> surfaces* and *how to run them locally*.
>
> **What this doc is not.**  The verification ledger.  Per-feature
> pass/fail status (⏳ untested / 🟡 partial / ✅ verified / ❌ failing)
> lives in [`docs/test-plan.md`](test-plan.md).  Real-hardware HIL
> contract lives in [`ci/HW-IN-LOOP.md`](../ci/HW-IN-LOOP.md).

---

## Quick start (from a fresh clone)

```bash
git clone https://github.com/alplabai/alp-sdk
cd alp-sdk

# One-time setup: Zephyr workspace + Python deps + apt hints.
bash scripts/bootstrap.sh

# Make Zephyr reachable for builds:
export ZEPHYR_BASE="$PWD/../zephyrproject/zephyr"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr

# Run every locally-runnable test (no HIL).
bash scripts/test-all.sh
```

A green run proves:

- Every `<alp/...>` public function compiles + links across the
  Yocto and baremetal plain-CMake backends.
- Every Zephyr-side wrapper passes its ZTEST failure-path suite
  under `native_sim/native/64`.
- Every Yocto-side wrapper passes its ctest failure-path suite.
- Every cryptographic primitive in `<alp/security.h>` passes its
  KAT + AEAD round-trip on OpenSSL.
- Every shipped example app builds + runs under twister.
- clang-format is clean on the diff.
- `board.yaml` metadata validates against its schema.
- Doxygen builds with zero warnings.

It does **not** prove:

- Any peripheral works against real silicon.  That's HIL territory
  (see `docs/test-plan.md`; ⏳ / 🟡 rows gate on the `hil-yocto`
  + `nightly-aen-hil` runners).
- Performance or memory budgets.  See `bench/` for those.

---

## Layers of verification

```
┌────────────────────────────────────────────────────────────────┐
│ HIL: real silicon, real broker, real sensor                    │
│   nightly-aen-hil.yml + hil-yocto (parked, ci/HW-IN-LOOP.md)   │
│   Flips test-plan.md rows from 🟡 → ✅.                         │
├────────────────────────────────────────────────────────────────┤
│ CI: GitHub-hosted runners, no hardware                         │
│   pr-twister + pr-plain-cmake + pr-static-analysis +           │
│   pr-generated-files + pr-metadata-validate + pr-doxygen +     │
│   coverity (weekly)                                            │
│   VS Code extension CI runs in alplabai/alp-sdk-vscode.        │
│   Necessary but not sufficient to tag a release.               │
├────────────────────────────────────────────────────────────────┤
│ Local: developer workstation, no hardware                      │
│   scripts/test-all.sh wraps every locally-runnable stage.      │
│   Same coverage as CI; runs in ~3 min on a warm cache.         │
└────────────────────────────────────────────────────────────────┘
```

---

## Coverage map per public header

| Header                            | Local test(s)                                                                                                         | HIL gate (flips to ✅)                |
|-----------------------------------|------------------------------------------------------------------------------------------------------------------------|---------------------------------------|
| `<alp/peripheral.h>` — I²C        | `tests/yocto/peripheral_i2c.c` + `tests/zephyr/peripheral/` (`smoke`)                                                  | `nightly-aen-hil` + `hil-yocto`       |
| `<alp/peripheral.h>` — SPI        | `tests/yocto/peripheral_spi.c` + `tests/zephyr/peripheral/`                                                            | `nightly-aen-hil` + `hil-yocto`       |
| `<alp/peripheral.h>` — UART       | `tests/yocto/peripheral_uart.c` + `tests/zephyr/peripheral/` + `examples/uart-echo/`                                   | `nightly-aen-hil` + `hil-yocto`       |
| `<alp/peripheral.h>` — UART RX ringbuf | `tests/zephyr/peripheral/` (`uart_rx_ringbuf` scenario) + `examples/uart-rx-ringbuf/`                              | `nightly-aen-hil`                     |
| `<alp/peripheral.h>` — GPIO       | `tests/yocto/peripheral_gpio.c` + `tests/zephyr/peripheral/` + `examples/gpio-button-led/`                             | `nightly-aen-hil` + `hil-yocto`       |
| `<alp/pwm.h>`                     | `tests/zephyr/peripheral/` (`caps_e3` scenario) + `examples/pwm-led-fade/`                                             | `nightly-aen-hil`                     |
| `<alp/adc.h>`                     | `tests/zephyr/peripheral/` + `examples/adc-voltmeter/`                                                                 | `nightly-aen-hil`                     |
| `<alp/counter.h>`                 | `tests/zephyr/peripheral/` + `examples/counter-alarm/` + `examples/qenc-readout/`                                      | `nightly-aen-hil`                     |
| `<alp/i2s.h>`                     | `tests/zephyr/peripheral/` + `examples/i2s-tone/`                                                                      | `nightly-aen-hil`                     |
| `<alp/can.h>`                     | `tests/zephyr/peripheral/` + `examples/can-loopback/`                                                                  | `nightly-aen-hil`                     |
| `<alp/rtc.h>`                     | `tests/zephyr/peripheral/` + `examples/rtc-clock/`                                                                     | `nightly-aen-hil`                     |
| `<alp/wdt.h>`                     | `tests/zephyr/peripheral/` + `examples/wdt-feed/`                                                                      | `nightly-aen-hil`                     |
| `<alp/audio.h>` (Zephyr)          | `tests/zephyr/audio/` + `examples/audio-loopback/`                                                                     | `nightly-aen-hil`                     |
| `<alp/audio.h>` (Yocto, ALSA)     | `tests/yocto/audio_alsa.c` — 11 failure paths                                                                          | `hil-yocto`                           |
| `<alp/iot.h>` — MQTT cleartext    | `tests/yocto/iot_mqtt.c` (parse + open) + `tests/zephyr/iot/`                                                          | `hil-yocto` + `nightly-aen-hil`       |
| `<alp/iot.h>` — MQTT TLS          | `tests/yocto/iot_mqtt.c` (5 TLS tests: default / pinned-CA / missing-CA / insecure / default-port-8883)                | `hil-yocto`                           |
| `<alp/security.h>` (Zephyr)       | `tests/zephyr/security/`                                                                                               | `nightly-aen-hil`                     |
| `<alp/security.h>` (Yocto, OpenSSL) | `tests/yocto/security_openssl.c` — 16 tests: SHA-256 NIST `"abc"` KAT, SHA-384/512 length, AEAD round-trip, tag-mismatch, key-length / NULL refusals, TRNG fill | meta-alp image build (flips ✅)      |
| `<alp/ble.h>`                     | `tests/zephyr/ble/`                                                                                                    | `nightly-aen-hil`                     |
| `<alp/mproc.h>` — shmem/mbox/hwsem | `tests/zephyr/mproc/` (`smoke` scenario)                                                                              | `nightly-aen-hil`                     |
| `<alp/mproc.h>` — IPC framing     | `tests/zephyr/mproc/` (`nanopb_framing` scenario, 9 ZTESTs)                                                            | `nightly-aen-hil`                     |
| `<alp/inference.h>`               | `tests/zephyr/inference/` + `tests/yocto/inference_dispatcher.c`                                                       | `nightly-aen-hil` + `hil-yocto`       |
| `<alp/usb.h>`                     | `tests/zephyr/usb/`                                                                                                    | `nightly-aen-hil`                     |
| `<alp/hw_info.h>`                 | `tests/zephyr/hw_info/`                                                                                                | `nightly-aen-hil` + production-test bench |
| `<alp/display.h>` / `<alp/gui.h>` / `<alp/camera.h>` / `<alp/storage.h>` | compile-only via `tests/smoke.c` + headers-include test                                              | (real impls pending)                  |
| Chip drivers (`chips/*/`)         | `tests/zephyr/chips/` with fakes for `lsm6dso`, `bme280`, `ssd1306`                                                    | per-chip on `nightly-aen-hil`         |
| `<alp/soc_caps.h>` generation     | `pr-generated-files.yml` (drift gate)                                                                                  | n/a (generator-deterministic)         |
| ABI snapshot                      | `scripts/abi_snapshot.py` + `docs/abi/v0.1-snapshot.json` (drift gate)                                                 | n/a                                   |
| `board.yaml` schema + loader      | `pr-metadata-validate.yml` smoke + `tests/scripts/test_alp_project.py`                                                 | n/a                                   |

---

## How to run a single layer

### Yocto / plain-CMake only

```bash
cmake -B build/yocto -S . -DALP_OS=yocto -DALP_BUILD_TESTS=ON
cmake --build build/yocto --parallel
ctest --test-dir build/yocto --output-on-failure
```

Optional native libs unlock the Yocto-side wrappers (otherwise they
stay at the NOSUPPORT stubs and the matching tests skip):

```bash
sudo apt-get install -y libmosquitto-dev libasound2-dev libssl-dev pkg-config
```

### Zephyr / twister only

```bash
export ZEPHYR_BASE="$PWD/../zephyrproject/zephyr"
python3 "$ZEPHYR_BASE/scripts/twister" \
    --testsuite-root tests/zephyr \
    --testsuite-root examples \
    -p native_sim/native/64 \
    --inline-logs --no-detailed-test-id
```

### Baremetal (compile-only)

```bash
cmake -B build/baremetal -S . -DALP_OS=baremetal
cmake --build build/baremetal --parallel
# No tests at this layer yet -- the v0.1 baremetal backend is stubs.
# Real-impl tests land alongside the bare-metal AEN port.
```

### Just the new feature scenarios

```bash
# LwRB UART RX ringbuf (feature-on compile + ZTEST run)
twister --testsuite-root tests/zephyr -p native_sim/native/64 \
    -T tests/zephyr/peripheral -s alp_sdk.peripheral.uart_rx_ringbuf

# nanopb mproc IPC framing
twister --testsuite-root tests/zephyr -p native_sim/native/64 \
    -T tests/zephyr/mproc -s alp_sdk.mproc.nanopb_framing
```

### Static analysis

```bash
# One-time: pin local clang-format to v14 (the version CI uses).
bash scripts/setup-clang-format.sh

# Diff-only clang-format (matches CI's pr-static-analysis behaviour).
git diff -U0 HEAD~1 -- '*.c' '*.h' | clang-format-diff -p1
```

Skipping the pin step is the single most common cause of green-locally /
red-in-CI on the `pr-static-analysis` job -- clang-format v18+ reflows a
handful of constructs that v14 doesn't.  See
[`docs/contribution.md`](contribution.md#formatting) for the divergence
list.

---

## CI ↔ local correspondence

Every CI workflow has a local counterpart that runs the same coverage:

| CI workflow                    | Local equivalent                                          |
|--------------------------------|-----------------------------------------------------------|
| `pr-plain-cmake.yml`           | `bash scripts/test-all.sh --yocto-only`                   |
| `pr-twister.yml`               | `bash scripts/test-all.sh --zephyr-only`                  |
| `pr-static-analysis.yml`       | `bash scripts/test-all.sh` (clang-format-diff stage)      |
| `pr-generated-files.yml`       | `python3 scripts/gen_soc_caps.py --check`                 |
| `pr-metadata-validate.yml`     | `python3 scripts/validate_metadata.py` + alp_project.py   |
| `pr-doxygen.yml`               | `doxygen Doxyfile` (zero-warnings)                        |
| (extension CI lives in `alplabai/alp-sdk-vscode`) | `cd ../alp-sdk-vscode && npm test`                     |
| `coverity.yml`                 | none (Coverity Scan only)                                 |

The plain run of `scripts/test-all.sh` covers everything except
Coverity and pr-generated-files (those are
narrow drift checks and run automatically in CI).

---

## Per-feature verification policy

Hardware-bound features ship `⏳ untested` and stay there until HIL
evidence lands.  Software-bound features (parsers, framing, KATs)
can flip to ✅ on a deterministic test alone -- see the security
backend row in [`docs/test-plan.md`](test-plan.md) for an example.

The full status legend is at the top of `docs/test-plan.md`.

---

## Adding tests for new features

The convention every new feature follows:

1. **Public surface change.**  Add doxygen-commented declarations to
   the matching `<alp/...>` header.
2. **Failure-path tests.**  Append ZTESTs (Zephyr) or `test_*` functions
   (Yocto / plain-CMake) covering NULL args, INVAL inputs, NOSUPPORT
   contracts.
3. **Test-plan row.**  Append a row in `docs/test-plan.md` with
   default status `⏳ untested` (or `🟡 partial` if failure paths are
   covered and the happy path runs on the local runner).
4. **Coverage map row.**  Add an entry to the table above mapping
   the new function to its test(s).
5. **CHANGELOG entry.**  Document the feature under `[Unreleased]`.

The matching pull request template is at `.github/PULL_REQUEST_TEMPLATE.md`
(when it lands).  The `CONTRIBUTING.md` walks through the same checklist.

---

## See also

- [`docs/test-plan.md`](test-plan.md) — per-feature verification status.
- [`ci/HW-IN-LOOP.md`](../ci/HW-IN-LOOP.md) — HIL runner contract.
- [`CONTRIBUTING.md`](../CONTRIBUTING.md) — full contributor workflow.
- [`docs/secure-boot.md`](secure-boot.md) — chain of trust + key
  lifecycle (the OTA + secure-boot half of the verification story).
