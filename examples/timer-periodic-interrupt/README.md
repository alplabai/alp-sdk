# timer-periodic-interrupt

The canonical "periodic ISR plus main-thread coordination"
example.

* Free-running counter + 100 ms re-arming alarm.
* ISR sets a flag; main thread drains the flag and toggles a
  GPIO LED.
* Teaches the recommended ISR-safety pattern: never call
  `printf` / `LOG_*` from an ISR -- defer to the main thread
  via a `volatile bool` flag (or a `k_event` / `k_msgq` for
  higher fan-out).

## What this shows

* `alp_counter_open()` + `alp_counter_start()` -- bring up a
  free-running counter.
* `alp_counter_us_to_ticks()` -- portable microsecond -> tick
  conversion that hides the per-SoM tick rate.
* `alp_counter_set_alarm()` -- one-shot deadline + IRQ callback.
* **Periodic out of one-shot**: the alarm callback re-arms itself
  for the next period.
* `alp_gpio_open()` + `alp_gpio_configure()` + `alp_gpio_write()`
  -- LED toggle from the main thread.  The EVK has no plain GPIO
  LED, so the indicator is the RGB-red pad (default function PWM3)
  claimed as a digital GPIO via `E1M_GPIO_PWM3` -- the e1m-spec
  "GPIO secondary" capability.
* The "flag + main-thread drain" coordination pattern.
* Clean shutdown via `alp_counter_cancel_alarm()` +
  `alp_counter_close()`.

## ISR safety primer

What you CAN call from an alarm callback (ISR context):

* `alp_gpio_write()`, `alp_gpio_read()` -- register access only,
  no blocking.
* `alp_counter_set_alarm()`, `alp_counter_get_value()` -- ditto.
* Zephyr's `printk()` (slow, but safe).
* Atomic ops (`atomic_inc`, `atomic_set`).

What you must NOT call from an ISR:

* `printf()`, `LOG_INF()`, `LOG_DBG()` (take locks / allocate).
* `alp_i2c_*`, `alp_spi_*`, `alp_uart_*` (blocking I/O).
* `k_sleep()`, `k_msleep()`, `k_mutex_lock()` (block the ISR).
* `k_malloc()` and other allocation primitives.

When in doubt: defer to a worker thread or main loop via a flag
or message queue.

## Build

```bash
# Standalone, native_sim (no counter -- exits early with diagnostic):
west build -b native_sim/native/64 examples/timer-periodic-interrupt \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run

# On real silicon, point -b at the SoM's Zephyr board target.
# Example for E1M-AEN701:
west build -b alp_e1m_aen701_m55_hp examples/timer-periodic-interrupt
west flash
```

## Expected output

native_sim (no counter device):

```
[timer] open counter=0
[timer] open counter failed: alp_last_error=-2
[timer] done
```

Real hardware (AEN, EVK with the RGB-red LED on the PWM3 pad as GPIO):

```
[timer] open counter=0
[timer] start -> 0
[timer] 100000 us = 1600 ticks (status=0)
[timer] open LED on E1M_GPIO_PWM3
[timer] arming first alarm
[timer] tick 0 fired @ 1600 ticks, LED -> 1
[timer] tick 1 fired @ 3200 ticks, LED -> 0
[timer] tick 2 fired @ 4800 ticks, LED -> 1
[timer] tick 3 fired @ 6400 ticks, LED -> 0
[timer] tick 4 fired @ 8000 ticks, LED -> 1
[timer] done
```

LED on the EVK blinks at 5 Hz (200 ms full period -- toggles
every 100 ms).

V2N supervisor backend:

```
[timer] open counter=0
[timer] start -> 0
[timer] us_to_ticks not supported on this backend; this example is AEN / native_sim today
[timer] done
```

(The GD32 IO MCU has no interrupt line back to the Renesas host
on V2N, so alarm callbacks fired in GD32 firmware ISR context
can't be relayed across the bridge in bounded time.  Run this
example on AEN for the working ISR path.)

## Customising

* **Different period.**  Drop `ALARM_PERIOD_US` to 10000 for
  100 Hz toggle (visible LED breathe); raise to 1000000 for
  1 Hz (a slow blink for "is it alive?" checks).
* **Different LED pin.**  Change `E1M_GPIO_PWM3` to whatever pad
  your board wires to an LED (a plain `E1M_GPIO_IO<N>` on a board
  with a dedicated GPIO LED, or any free GPIO to scope-probe).
* **No LED at all.**  Drop the GPIO block; the printf trace alone
  proves the alarm is firing.

## Reference

- [`<alp/counter.h>`](../../include/alp/counter.h) -- counter + qenc API.
- [`<alp/peripheral.h>`](../../include/alp/peripheral.h) -- GPIO API.
- [`<alp/e1m_pinout.h>`](../../include/alp/e1m_pinout.h) -- the
  `E1M_GPIO_<class><N>` pin-as-GPIO indices.
- [`examples/counter-alarm/`](../counter-alarm/)
  -- single-shot alarm sibling.
- [`examples/gpio-button-led/`](../gpio-button-led/)
  -- GPIO basics + the same pin-as-GPIO trick.
- Zephyr `k_event` / `k_msgq` docs -- richer ISR -> thread
  coordination primitives when a single bool flag isn't enough.
