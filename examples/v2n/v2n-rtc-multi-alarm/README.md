# v2n-rtc-multi-alarm

Exercises the RV-3028-C7's **multi-source event dispatcher**: the
RTC has seven distinct interrupt sources (PORF, EXT_EVENT, ALARM,
COUNTDOWN, PERIODIC, BSF, CLKF) that all latch into a single
STATUS register and route to one physical INT pin.  The driver
fans the single IRQ out into per-source callbacks so the
application's state-machine cleanly separates concerns.

## What it shows

1. `rv3028c7_register_handler(...)` for each source the app cares
   about (PORF, ALARM, PERIODIC, BSF in this example).
2. `rv3028c7_set_int_enable(...)` to mask the chip's INT pin per
   source.
3. `rv3028c7_set_alarm(...)` configures a minute-match alarm.
4. `rv3028c7_dispatch_irq(...)` reads STATUS, invokes registered
   handlers, write-0-to-clears every fired flag.
5. `rv3028c7_route_clkout(...)` reprograms the CLKOUT pin to act
   as a second physical interrupt line tied to a specific source.

## Wiring it to a real IRQ

In production firmware the board side wires the RTC's INT pin
to a host GPIO, registers an ISR, and posts a work-queue item
that calls `rv3028c7_dispatch_irq` from thread context.  This
example calls `dispatch_irq` directly from `main()` to show the
flow without requiring you to wire the GPIO first.

## See also

* [`<alp/chips/rv3028c7.h>`](../../../include/alp/chips/rv3028c7.h)
  -- driver API.
* Micro Crystal application note "Multiple Interrupt Lines with
  RV-3028-C7" -- the design rationale behind the CLKOUT-as-second-
  IRQ pattern.
