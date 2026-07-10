# src/ArduinoCompat/

The small Arduino-Stream shim this example provides: `Print.h`,
`Stream.h`, `Printable.h`. Named to match the filenames TinyGSM's own
`src/ArduinoCompat/Client.h` (in the `tinygsm` module,
`modules/lib/tinygsm/src/ArduinoCompat/Client.h`) and
`src/ArduinoCompat/IPAddress.h` quote-`#include` when the module is
built with `-DARDUINO_DASH` -- TinyGSM's own opt-in switch for
non-Arduino platforms (see `TinyGsmCommon.h`).

**This is deliberately small and does not get TinyGSM itself to
compile.** It was built by iteratively compiling
`#include <TinyGsmClient.h>` in isolation (`g++ -DARDUINO_DASH
-DTINY_GSM_MODEM_SIM800`) and supplying exactly the headers the
compiler asked for, in order:

1. `ArduinoCompat/Client.h` (module-provided) needs `Print.h` and
   `Stream.h` -- supplied here.
2. `ArduinoCompat/IPAddress.h` (module-provided) needs `Printable.h`
   -- supplied here.
3. `ArduinoCompat/IPAddress.h` then needs **`WString.h`** -- Arduino's
   `String` class header. **This is where the chain stops.** `String`
   is not a small class (it's Arduino's dynamically-resizing string
   type with ~40 methods: concatenation, `indexOf`/`lastIndexOf`,
   `substring`, `toInt`/`toFloat`, `trim`, `replace`, `startsWith`/
   `endsWith`, ...) and `TinyGsmModem.tpp` plus every per-modem header
   (`TinyGsmClientSIM800.h` and the `.tpp`s it pulls in for battery /
   calling / GPRS / GPS / SMS / SSL / TCP / time / NTP) use it
   pervasively -- not just for `getModemInfo()`'s return value, but
   throughout the AT-response parser (`waitResponse()`'s `String
   data` accumulator, `data.endsWith(...)`, `data.substring(...)`,
   ...). Reimplementing enough of `String` to satisfy every call site
   is a full Arduino-core-compatibility project, not "a small
   Arduino-Stream shim" -- see `../../README.md` "Why this doesn't
   build" for the full reasoning and the escape valve this example
   takes instead.

If you're wiring a real UART-backed `Stream` for TinyGSM on a
different platform that already ships a `WString.h` (e.g. an
ESP-IDF/Arduino-core environment), these three files are unnecessary
-- use the real Arduino core headers instead of this shim.
