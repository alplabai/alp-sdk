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
| `chips/`                | 108         | `tests/zephyr/chips/src/main.c` (2107 LOC) | 21 chip drivers + 3 fakes (`lsm6dso`, `bme280`, `ssd1306`)         |
| `peripheral/` (13 APIs) | 82          | `tests/zephyr/peripheral/src/main.c` (902 LOC) | Single monolithic file covering I²C/SPI/UART/GPIO/PWM/ADC/CAN/I²S/RTC/WDT/DAC/QENC/COUNTER |
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

`tests/zephyr/peripheral/src/main.c` is a single file with one
`ZTEST_SUITE(alp_peripheral, ...)` and 82 ZTESTs.  Counts by
`<alp/X.h>` surface (matched on `test_<peri>_` prefix):

| Peripheral | ZTESTs | Surface coverage health |
|------------|--------|-------------------------|
| `pwm`      | 9      | ✅ healthy (open/close, configure, set/get, single-pulse, capture) |
| `adc`      | 8      | ✅ healthy (open/close, configure, single-shot, streaming, DSP)    |
| `uart`     | 6      | ✅ healthy (open/close, write, RX ringbuf attach/pop)              |
| `i2c`      | 4      | 🟡 lifecycle only -- no positive write/read path                    |
| `gpio`     | 4      | 🟡 lifecycle + IRQ-args -- no edge-detect path                      |
| `dac`      | 3      | 🟡 thin -- no waveform path                                         |
| `can`      | 3      | 🟡 thin -- no TX/RX exchange path                                   |
| `wdt`      | 2      | 🔴 very thin -- no feed-loop path                                   |
| `spi`      | 2      | 🔴 very thin -- no MISO/MOSI roundtrip                              |
| `i2s`      | 2      | 🔴 very thin -- no stream-out path                                  |
| `rtc`      | 1      | 🔴 single test -- needs at least open/set/get/alarm                 |
| `qenc`     | 1      | 🔴 single test                                                      |
| `counter`  | 1      | 🔴 single test                                                      |

**Sum of named-peripheral tests: 46.**  The other 36 ZTESTs in
the file cover cross-cutting concerns (`alp_last_error()`,
peripheral-config arg-validation, NULL-handle lifecycle on
shared types) that don't map to a single peripheral.

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

### 1. Split `tests/zephyr/peripheral/src/main.c`

902-line monolith.  Splitting into `tests/zephyr/peripheral/src/{i2c,spi,uart,gpio,pwm,adc,dac,can,i2s,rtc,wdt,qenc,counter}.c` makes the per-peripheral test counts visible from the filesystem, lets developers run a single peripheral's suite, and creates the natural insertion point for the gaps below.

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
# Per-area total
grep -c '^ZTEST' tests/zephyr/<area>/src/main.c

# Per-peripheral inside peripheral/main.c
grep -oE 'test_(i2c|spi|gpio|uart|pwm|adc|can|i2s|rtc|wdt|usb|dac|qenc|counter)_' \
    tests/zephyr/peripheral/src/main.c | sort | uniq -c
```

Re-running both shows whether a future change moved the
numbers.  This doc should be refreshed alongside any per-area
test additions.
