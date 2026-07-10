# tinygsm-modem-at

Runs the cellular-modem AT bring-up flow [TinyGSM](https://github.com/vshymanskyy/TinyGSM)
drives -- an `AT` liveness probe, then `ATI` / `AT+CGMI` / `AT+GMM`
for `getModemInfo()` -- against a mock AT-transcript `Stream`, and
prints the assembled modem info. No real UART or cellular modem
required.

**[UNTESTED]** -- builds and runs on `native_sim` (mock transcript);
not bench-run against a real modem. It drives the AT exchange
*directly* over the mock `Stream` rather than linking the upstream
TinyGSM library -- see "Why the real library isn't linked" below for
the reason, and `src/main.cpp` for the real `TinyGsm modem(stream)`
call sequence it mirrors.

## Why the real library isn't linked

TinyGSM (`modules/lib/tinygsm`, `west.yml`'s `extras-tier1` group) is
written against the Arduino core and ships no `zephyr/module.yml`, so
(like `minimp3-decode`'s vendored header and `libhelix-decode`) its
include directory isn't added automatically -- this example's
`CMakeLists.txt` wires it explicitly.

TinyGSM's own `TinyGsmCommon.h` has an opt-in switch for exactly this
situation -- non-Arduino platforms building it standalone:

```c
#if defined(ARDUINO_DASH)
#include <ArduinoCompat/Client.h>
#else
#include <Client.h>
#endif
```

`-DARDUINO_DASH` (set in this example's `CMakeLists.txt`) routes
through the module's own bundled `ArduinoCompat/Client.h` instead of
a real Arduino core's `<Client.h>`. This example supplies the three
headers *that* file needs (`src/ArduinoCompat/Print.h`,
`Stream.h`, `Printable.h` -- the "small Arduino-Stream shim" the
parent plan asks for). That was built by iteratively compiling
`#include <TinyGsmClient.h>` in isolation and supplying exactly what
the compiler asked for, in order:

```
$ g++ -std=c++17 -Isrc/ArduinoCompat -I<tinygsm>/src -DARDUINO_DASH -DTINY_GSM_MODEM_SIM800 \
      -c -xc++ -o /dev/null - <<< '#include <TinyGsmClient.h>'

modules/lib/tinygsm/src/ArduinoCompat/Client.h needs Print.h, Stream.h    -- supplied
modules/lib/tinygsm/src/ArduinoCompat/IPAddress.h needs Printable.h      -- supplied
modules/lib/tinygsm/src/ArduinoCompat/IPAddress.h needs WString.h        -- STOP
```

`WString.h` is Arduino's `String` class -- a dynamically-resizing
string type with ~40 methods (concatenation, `indexOf`/`lastIndexOf`,
`substring`, `toInt`, `trim`, `replace`, `startsWith`/`endsWith`, ...)
that TinyGSM's AT-response parser (`TinyGsmModem.tpp`'s
`waitResponse()`) and every per-modem header this build would need
(`TinyGsmClientSIM800.h` plus the battery/calling/GPRS/GPS/SMS/SSL/
TCP/time/NTP `.tpp` files it pulls in) use pervasively -- not just for
`getModemInfo()`'s return value. Reimplementing enough of `String` to
satisfy every call site is a full Arduino-core-compatibility project,
not "a small Arduino-Stream shim" -- see
[`src/ArduinoCompat/README.md`](src/ArduinoCompat/README.md) for the
full accounting.

## What this builds instead

`src/main.cpp` drives the **same AT exchange TinyGSM would** --
`init()`'s `AT` liveness probe, then `getModemInfo()`'s `ATI` /
`AT+CGMI` / `AT+GMM` sequence with the three payload lines joined into
one info string -- directly over the mock `Stream`, with no dependency
on Arduino's `String`. So it builds and runs on `native_sim`. The
**real, library-driven** form of exactly this flow --
`TinyGsm modem(stream); modem.init(); modem.getModemInfo();` -- is
written out in `src/main.cpp`'s banner, ready to swap in on a target
that supplies a full Arduino `String`. The mock
(`src/mock_at_stream.h`) presents the same `Stream&` contract a real
UART would, so that swap changes only what backs the stream.

## Swapping in a real UART

On real hardware, replace `MockAtStream` with a small `Stream`
adapter over this SDK's portable `<alp/uart.h>` handle
(`alp_uart_open()` + the board's modem UART instance ID), keeping
`TinyGsm modem(stream)` and every call after it unchanged --
`board.yaml` would add the `uart` peripheral and the SoM-specific pin
macro (see e.g. [`examples/peripheral-io/uart-echo`](../../peripheral-io/uart-echo)
for that pattern). None of TinyGSM's own API changes; only what backs
`Stream&` does. That swap doesn't unblock the `WString.h` gap above --
both native_sim and real silicon need it resolved the same way.

## Reference

- [`src/ArduinoCompat/README.md`](src/ArduinoCompat/README.md) -- the
  shim's exact scope and the full compiler-error chain that motivates
  it.
- [`metadata/library-profiles/tinygsm/hw-backends.yaml`](../../../metadata/library-profiles/tinygsm/hw-backends.yaml)
  -- the UART-DMA/sync-IO backend selection this example's
  `libraries: [tinygsm]` wires (unused by this example's mock-Stream
  path, but exercised once a real UART is added).
