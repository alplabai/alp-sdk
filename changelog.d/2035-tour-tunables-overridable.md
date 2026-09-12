### Fixed — `aen-cc3501e-companion-tour`'s timeouts are actually `-D`-overridable now (#2035)

`TOUR_CONNECT_TIMEOUT` and its four neighbours were bare `#define`s, sitting
directly beneath the `#ifndef`-guarded `TOUR_WIFI_SSID` / `TOUR_WIFI_PASS` /
`TOUR_WIFI_SECURITY`. A bare `#define` silently wins over a command-line
`-D`, so the build flag was accepted and then ignored.

That cost a bench run. A run built with `-DTOUR_CONNECT_TIMEOUT=60000u`
compiled `15000u` in anyway, and its failed association could not be told
apart from a genuine one — the question "does the connect just need more
time?" went untested while looking tested.

All five are now `#ifndef`-guarded, matching the credentials beside them.
The comment also records why the default is deliberately below the
firmware's own 40 s worst case for a station connect: this app is a quick
full-surface tour, not a connection test, and a tour that parks for 40 s on
one step is not a tour. Override it for a real association attempt — the
sibling `aen-cc3501e-socket-throughput` budgets 55000u for exactly that.
