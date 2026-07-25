# board-selftest

The app you run **first** on a freshly-assembled board, before
chasing a peripheral that "doesn't work". It answers the four
questions every bring-up starts with and prints a single pass/fail
report a field tech can read off a serial console without a
debugger.

| # | Check | Portable API |
|---|-------|--------------|
| 1 | Is this the SoM I think it is? | `alp_hw_info_read()` (on-module EEPROM identity) |
| 2 | Is the silicon alive + answering? | `alp_soc_secure_fw_ping()` + `alp_soc_info_read()` |
| 3 | Is the core rail/clock sane? | `alp_power_profile_get(ALP_POWER_PROFILE_RUN, ...)` |
| 4 | What's actually on the I2C bus? | `alp_i2c_read()` 7-bit address scan |

## What this shows

* **Portable, chip-free.** Only `<alp/*>` headers -- no chip driver,
  no vendor header -- so the *same* `src/main.c` builds and runs on
  every E1M family (a Ring 1 example per
  [`docs/portability.md`](../../../docs/portability.md)).
* **SKIP is not FAIL.** A check whose backend has no probe returns
  `ALP_ERR_NOSUPPORT`, which the self-test reports as **SKIP** -- "this
  backend can't answer", not "the hardware is broken". Conflating a
  missing capability with a fault is the one thing a self-test must
  never do.
* **Read-only.** The power check reads the RUN operating-point
  profile; it never calls `alp_power_profile_set()` (which *changes*
  the live rail/clock -- wrong here, and dangerous on hardware).

## Hardware

Runs on any E1M SoM. The I2C scan walks `BOARD_I2C_SENSORS` -- the
`<alp/board.h>` alias that resolves to `ALP_E1M_I2C0` on the E1M EVK
and `ALP_E1M_X_I2C0` on the E1M-X EVK. That is also the bus the
on-module EEPROM identity manifest lives on. Rebind it in
`board.yaml` `pins:` to port to another board.

## Build

```bash
# Standalone, native_sim (emul I2C; identity/power checks SKIP):
west build -b native_sim/native/64 examples/bringup/board-selftest \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run

# On real silicon, point -b at the SoM's Zephyr board target.
# Example for E1M-AEN801:
west build -b alp_e1m_aen801_m55_hp examples/bringup/board-selftest
west flash
```

## Expected output

Real hardware (E1M-AEN801, all checks answer):

```
[selftest] === board self-test ===
[selftest] SoM identity: E1M-AEN801 rev r1 sn AEN0000123 -> PASS
[selftest] SoC identity: alif:ensemble:e8 (secure-fw OK) -> PASS
[selftest] power profile: RUN core 800 mV @ 400 MHz -> PASS
[selftest] i2c scan BOARD_I2C_SENSORS: 3 device(s) -> PASS
[selftest] result: 4 PASS, 0 SKIP, 0 FAIL
[selftest] done
```

native_sim (no EEPROM target / controller / radio; emul I2C bus is empty):

```
[selftest] === board self-test ===
[selftest] SoM identity: unreadable (ALP_ERR_NOT_READY) -> FAIL
[selftest] SoC identity: alif:ensemble:e8 (ping ALP_ERR_NOSUPPORT, read ALP_ERR_NOSUPPORT) -> SKIP
[selftest] power profile: unavailable (ALP_ERR_NOSUPPORT) -> SKIP
[selftest] i2c scan BOARD_I2C_SENSORS: probing 0x08..0x77
[selftest] i2c scan BOARD_I2C_SENSORS: 0 device(s) -> PASS
[selftest] result: 1 PASS, 2 SKIP, 1 FAIL
[selftest] done
```

The SoM-identity **FAIL** on native_sim is expected and honest: the
emul I2C bus is up, but no EEPROM target is registered, so
`alp_hw_info_read()` returns `ALP_ERR_NOT_READY` (an unreadable
identity manifest) rather than `ALP_ERR_NOSUPPORT` (no bus at all).
The check can't tell "no EEPROM on this sim" from "broken EEPROM on
real silicon" -- and on hardware a NAKing manifest genuinely is a
fault -- so it reports FAIL either way. The `[selftest] done` marker
still latches, so the twister console harness passes regardless.

## Troubleshooting

* **SoM identity `ALP_ERR_NOT_PROVISIONED`.** The on-module EEPROM
  reads back blank -- the module was never run through
  `scripts/program_eeprom.py` at production test. On a factory-fresh
  board this is expected; on a shipped SoM it is a real fault.
* **SoM identity `ALP_ERR_IO`.** Magic present but the CRC/schema
  check failed -- a corrupt manifest. Re-program the EEPROM.
* **i2c scan `open failed`.** The `alp-i2c0` DT alias isn't set --
  check your board overlay, or for native_sim that `CONFIG_EMUL=y
  CONFIG_I2C_EMUL=y` and the overlay we ship are picked up.
* **A check reports FAIL (not SKIP).** The probe exists on this
  backend but the read went wrong -- that's a genuine board fault
  worth chasing, unlike a SKIP.

## Reference

- [`<alp/hw_info.h>`](../../../include/alp/hw_info.h) -- SoM/SoC identity surface.
- [`<alp/power.h>`](../../../include/alp/power.h) -- operating-point profile surface.
- [`<alp/peripheral.h>`](../../../include/alp/peripheral.h) -- I2C surface.
- [`examples/peripheral-io/i2c-scanner/`](../../peripheral-io/i2c-scanner/) -- standalone bus-scan companion.
