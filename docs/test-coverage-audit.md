# Test coverage audit — 2026-05-14

Audit pass over `tests/zephyr/<area>/` to map ZTEST counts vs
public API surface and flag gaps.  No new tests added in this
pass -- this is the matrix; per-area test additions are
follow-up work.

> **What this doc is.**  A snapshot of test counts per `<alp/...>`
> surface plus a "thin-coverage" list for prioritising future
> test work.
>
> **What this doc is not.**  Pass/fail status (lives in
> [`docs/test-plan.md`](test-plan.md)).  Run instructions (live
> in [`docs/testing.md`](testing.md)).

## Headline counts

| Area                    | ZTEST count | File                                       | Notes                                                              |
|-------------------------|-------------|--------------------------------------------|--------------------------------------------------------------------|
| `chips/`                | 254         | `tests/zephyr/chips/src/main.c` (~2700 LOC) | 80 chip drivers + 3 fakes (`lsm6dso`, `bme280`, `ssd1306`); 49 chips added in the v0.5 §D.AI/industrial/iot/audio batches ship with `[UNTESTED]` badges + NULL-arg-guard ZTESTs |
| `peripheral/` (13 APIs) | 82          | `tests/zephyr/peripheral/src/{main,i2c,spi,gpio,uart,pwm,adc,dac,counter,qenc,i2s,can,rtc,wdt}.c` | Split per peripheral in §C.16; `main.c` keeps cross-cutting tests (TMU, power, AEN audit gaps, delay, SoC caps, V2N supervisor, TRNG entropy) |
| `iot/`                  | 14          | `tests/zephyr/iot/src/main.c`              | WiFi + MQTT (cleartext + TLS) negative paths                       |
| `audio/`                | 9           | `tests/zephyr/audio/src/main.c`            | `audio_in` + `audio_out` lifecycle + arg validation                |
| `inference/`            | 8           | `tests/zephyr/inference/src/main.c`        | Backend-dispatcher rejection paths (CPU / Ethos-U / null cfg)      |
| `security/`             | 8           | `tests/zephyr/security/src/main.c`         | Hash + AEAD + TRNG no-backend paths                                |
| `usb/`                  | 6           | `tests/zephyr/usb/src/main.c`              | Device + host no-backend lifecycle                                 |
| `hw_info/`              | (file)      | `tests/zephyr/hw_info/src/main.c`          | EEPROM-manifest CRC + version validation                           |
| `mproc/`                | (file)      | `tests/zephyr/mproc/src/main.c`            | Shmem/mbox/hwsem + nanopb framing                                  |
| `ble/`                  | (file)      | `tests/zephyr/ble/src/main.c`              | BLE advertising / scanning surface                                 |
| `dsp/`                  | (file)      | `tests/zephyr/dsp/src/main.c`              | DSP chain primitives                                               |

## Per-peripheral breakdown inside `peripheral/`

`tests/zephyr/peripheral/src/` was a single `main.c` (902 LOC) with one
`ZTEST_SUITE(alp_peripheral, ...)` and 82 ZTESTs; split per peripheral
in §C.16.  Every `*.c` sibling now registers its ZTESTs against the
same suite.  Counts by `<alp/X.h>` surface (matched on `test_<peri>_`
prefix):

Counts after the §C.22 thin-spot fills:

| Peripheral | ZTESTs | Surface coverage health |
|------------|--------|-------------------------|
| `pwm`      | 9      | ✅ healthy (open/close, configure, set/get, single-pulse, capture) |
| `counter`  | 8      | ✅ healthy after §C.22 fills (every public function NULL-guarded)   |
| `adc`      | 7      | ✅ healthy (open/close, configure, single-shot, streaming, DSP)    |
| `spi`      | 7      | ✅ healthy after §C.22 fills (open/close, transceive/write/read guards) |
| `uart`     | 6      | ✅ healthy (open/close, write, RX ringbuf attach/pop)              |
| `qenc`     | 5      | ✅ healthy after §C.22 fills (open/close + get/reset position guards)|
| `rtc`      | 5      | ✅ healthy after §C.22 fills (open + set/get_time guards)          |
| `wdt`      | 5      | ✅ healthy after §C.22 fills (open + feed/disable guards)          |
| `i2c`      | 4      | 🟡 lifecycle only -- no positive write/read path (needs HiL)        |
| `gpio`     | 4      | 🟡 lifecycle + IRQ-args -- no edge-detect path (needs HiL)          |
| `i2s`      | 4      | ✅ healthy after §C.22 fills (open with INVAL paths on every arg)  |
| `dac`      | 3      | 🟡 thin -- no waveform path (HiL)                                   |
| `can`      | 2      | 🟡 thin -- no TX/RX exchange path (HiL)                             |

**Sum of named-peripheral tests: 69 (was 46).**  The other 38
ZTESTs in main.c cover cross-cutting concerns (`alp_last_error()`,
peripheral-config arg-validation, NULL-handle lifecycle on
shared types) that don't map to a single peripheral.

Positive-path tests (real device transfer correctness) remain
HiL-gated -- native_sim has no real PWM / ADC / I²C / SPI /
UART / CAN / I²S devices to exchange data with.  The §C.22
fills closed the binding-layer-contract gap; positive paths
land alongside the corresponding HiL bring-up rows in
`docs/test-plan.md`.

## Native_sim coverage shape

Most "thin" areas (audio, inference, iot, security, usb) follow
the same pattern: tests cover **negative paths only** because
native_sim has no real backend.  Typical test names:

- `*_open_no_backend_returns_null`
- `*_open_null_cfg_invalid`
- `*_lifecycle_null_handle_safe`

This is the **correct shape** for native_sim -- positive paths
require real hardware (HiL) or a simulator backend.  But it
means the green-on-native_sim signal is **necessary but not
sufficient** for any of those areas; the verification ledger
([`docs/test-plan.md`](test-plan.md)) is the authoritative
positive-path gate.

## Gaps worth addressing (prioritised)

Ranked by "ease of testing in native_sim" × "API-surface impact":

### 1. Split `tests/zephyr/peripheral/src/main.c` — DONE §C.16

Landed in §C.16.  `tests/zephyr/peripheral/src/{i2c,spi,uart,gpio,pwm,adc,dac,can,i2s,rtc,wdt,qenc,counter}.c` now sit alongside the slimmed-down `main.c` (which retains the cross-cutting sections + the `ZTEST_SUITE` declaration).  Per-peripheral test counts visible from the filesystem; the natural insertion points for gaps 2..4 below are now the matching `*.c` file.

### 2. RTC / QENC / COUNTER positive-path tests

The three 1-test peripherals.  All three have lifecycle ops
that native_sim's Zephyr CONFIG_RTC=y / CONFIG_QDEC=y /
CONFIG_COUNTER=y will compile against.  Adding open / configure /
set / get / close paths brings them up to the same coverage
level as the healthy peripherals.

### 3. SPI / I²S / WDT positive-path tests

The three 2-test peripherals.  SPI loopback (MOSI <-> MISO),
I²S streaming start/stop, WDT feed-then-let-expire are all
testable under native_sim with the right `CONFIG_*` knobs.

### 4. I²C / DAC / CAN positive-path tests

3-4 tests today; specifically missing TX/RX exchange or
waveform/frame validation.  Same approach as #3.

### 5. mproc, ble, dsp counts

Not breaking these out yet -- spot-check first that they
follow the established `ZTEST_SUITE` pattern; if so, count is
visible via `grep -c '^ZTEST' tests/zephyr/<area>/src/main.c`.

## What this audit does **not** cover

- The 40+ HiL rows in [`docs/test-plan.md`](test-plan.md) --
  those depend on real hardware that the SDK CI cannot
  simulate.  The audit focuses on what's gettable today in
  CI.
- The Twister scenario matrix.  Each Twister scenario in a
  `testcase.yaml` is a build-flag variant rather than a new
  ZTEST; counting them would double-count without adding signal.
- Yocto-side tests under `tests/yocto/*` -- those are ctest,
  not ZTEST.  Counts there are best read from the
  `tests/yocto/CMakeLists.txt`.

## Methodology

```bash
# Per-area total (areas that are still single-file)
grep -c '^ZTEST' tests/zephyr/<area>/src/main.c

# Per-peripheral after the §C.16 split: one file per peripheral
grep -c '^ZTEST' tests/zephyr/peripheral/src/*.c | sort

# Or grep across all files when an area is multi-file
grep -c '^ZTEST' tests/zephyr/<area>/src/*.c
```

Re-running both shows whether a future change moved the
numbers.  This doc should be refreshed alongside any per-area
test additions.
