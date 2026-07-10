# tinygsm-modem-at

Drives [TinyGSM](https://github.com/vshymanskyy/TinyGSM)'s `TinyGsm`
modem class against a mock AT-transcript `Stream` (`AT` -> `OK`,
`AT+CGMI` -> `SIMCOM`, ...) and prints what `init()` /
`getModemInfo()` report. No real UART or cellular modem required.

**[UNTESTED] -- does not build in this workspace.** See "Why this
doesn't build" below for the exact, empirically-confirmed reason, and
"What this teaches instead" for what you still get out of it.

## Why this doesn't build

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

## What this teaches instead

`src/main.cpp` writes out the **correct, real TinyGSM integration
pattern** regardless: construct `TinyGsm modem(stream)` over a
`Stream&`, call `modem.init()`, call `modem.getModemInfo()`. The mock
(`src/mock_at_stream.h`) replays a canned AT-command/response
transcript through that same `Stream&` interface a real UART would
present -- functionally the same contract, just table-driven instead
of silicon-driven.

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
