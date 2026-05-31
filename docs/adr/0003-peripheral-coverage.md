# 0003. Wrap 12 peripheral classes at v0.2, not just I2C/SPI/GPIO/UART

Status: Accepted
Date: 2026-05-10

## Context

The v0.1 SDK shipped wrappers for four peripheral classes: I2C, SPI,
GPIO, UART.  An MCU exposes ~15.  The user looked at the source tree
in v0.1 and said: "I only see 4, there are usually a lot more in an
MCU."

That's correct.  v0.1 covered the four classes the v0 alp-studio
demo blocks needed (button+LED, OLED, IMU).  But "the studio's first
demo blocks" is the wrong upper bound for the v1.0 unification SDK.
The right bound is "what every E1M-conformant SoM exposes that a
typical embedded application uses."

## Decision

Wrap **12 peripheral classes** at v0.2 — the v0.1 four plus eight
new ones:

| Class                | v0.1 | v0.2 | Notes                                                |
|----------------------|------|------|------------------------------------------------------|
| I2C                  | ✓    | ✓    | shipped                                              |
| SPI                  | ✓    | ✓    | shipped                                              |
| GPIO                 | ✓    | ✓    | shipped                                              |
| UART                 | ✓    | ✓    | shipped                                              |
| **PWM**              |      | ✓    | `PWM0..PWM7` reserved by e1m-spec                    |
| **ADC**              |      | ✓    | sensor reads; capability-validated against SoC max bits |
| **Counter / Timer**  |      | ✓    | wakeup alarms, tick reads                            |
| **Quadrature decoder** |    | ✓    | `ENC0..ENC3` reserved by e1m-spec                    |
| **I2S / SAI**        |      | ✓    | substrate for `<alp/audio.h>`                        |
| **CAN / CAN-FD**     |      | ✓    | RZ/V2N has 6 channels                                |
| **RTC**              |      | ✓    | wall-clock time                                      |
| **Watchdog**         |      | ✓    | safety timer                                         |

Deferred to a separate header (out of scope for v0.2's `<alp/*.h>`):

| Class               | Plan                                                          |
|---------------------|---------------------------------------------------------------|
| **USB**             | `<alp/usb.h>` v0.3+; large state machine, host + device       |
| **Ethernet**        | folds into `<alp/iot.h>` networking                           |
| **QSPI / OSPI**     | `<alp/storage.h>` v0.4+ over Zephyr's `flash_*`               |
| **MIPI CSI**        | folds into `<alp/camera.h>`'s real impl                       |
| **MIPI DSI**        | folds into `<alp/display.h>`'s real impl                      |

Each new class:
- Has a public header `<alp/<class>.h>` with `@brief` / `@param` /
  `@return` Doxygen on every function.
- Has a portable class dispatcher at `src/<class>_dispatch.c` that
  resolves the studio-supplied `*_id` via the `alp-<class>N` DT
  alias and routes through the backend registry (see
  [`docs/architecture/backend-registry.md`](../architecture/backend-registry.md)).
- Has at least one Zephyr backend at `src/backends/<class>/zephyr_drv.c`
  registered via `ALP_BACKEND_REGISTER` that forwards to Zephyr's
  matching driver class.
- Has a Kconfig opt-in at `CONFIG_ALP_SDK_PERIPH_<CLASS>` that
  defaults `y if <ZEPHYR_SUBSYS>` so consumers don't pay code-size
  for classes they don't use.
- Has a static handle pool with `CONFIG_ALP_SDK_MAX_<CLASS>_HANDLES`.
- Honours `alp_last_error()` on open failures (see
  [ADR 0002](0002-error-mechanism.md)).

## Alternatives

**A. Stay at 4 peripheral classes; declare the rest as v0.4 / v1.0
work.**  Rejected because:
- Apps moving from AEN to V2N or i.MX93 in v0.4 would have to wait
  for PWM/ADC wrappers — many studio blocks need them now.
- Studio codegen would have to emit raw Zephyr calls for the
  unwrapped classes; that breaks the OS-pivot story for those
  blocks.
- The user explicitly raised this as a gap; deferring would feel
  evasive.

**B. Wrap *every* class an MCU exposes — including USB, Ethernet,
QSPI, MIPI.**  Rejected because:
- USB alone is a very large state machine — its own header
  deserves a v0.3 milestone.
- Ethernet, QSPI, MIPI CSI/DSI each fold more naturally into a
  higher-level header (`<alp/iot.h>`, `<alp/storage.h>`,
  `<alp/camera.h>`, `<alp/display.h>`) rather than living in
  peripheral-primitive territory.
- Bundling them into v0.2 would balloon scope and miss the
  delivery cadence in `VERSIONS.md`.

**C. Fold all peripherals into `<alp/peripheral.h>` rather than
per-class headers.**  Rejected because:
- The header would balloon to ~1500 lines.
- Apps that don't use a given class still pull in its types and
  `#define`s.
- Doxygen output gets harder to navigate — one giant page vs. one
  page per class.

## Consequences

**Good:**
- The studio can target any of the 12 classes in v0.2 without
  touching the SDK roadmap.
- Each per-class header is small (≤ 250 lines) and Doxygen-clean.
- Per-class Kconfig opt-in keeps code size honest.
- Hand-written firmware can use the wrappers too — they're not
  studio-only.

**Bad / costs:**
- The repo grows by ~1500 lines of wrapper code + ~200 lines of
  Kconfig + ~300 lines of CMake / DT bindings.
- Each class needs a ztest fixture (or at least NULL-handling
  tests).  Some Zephyr emul drivers don't exist (PWM, Counter)
  for `native_sim`, so coverage is uneven.
- The DT-alias selection scheme assumes the studio (or the user)
  populates `alp-<class>N` aliases.  Boards that don't get them
  populated see a permissive default (open returns NULL with
  `ALP_ERR_NOT_READY`).

## See also

- `docs/architecture.md` — "Alp SDK libraries" table reflects the
  expanded surface.
- `VERSIONS.md` — v0.2 delivery row for "richer blocks + V2N
  intro" lists this as a deliverable.
- `include/alp/{pwm,adc,counter,i2s,can,rtc,wdt}.h` — the new
  surfaces.
