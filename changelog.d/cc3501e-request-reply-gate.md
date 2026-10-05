### Fixed — CC3501E SOCK_SEND of 400..610 bytes no longer desyncs the bridge link

On bridge firmware without `ALP_CC3501E_CAP_FAST_REPLY` (v0.9.0,
`fw_version` 0x0900), a `cc3501e_sock_send()` whose payload was roughly 390
to 610 bytes was executed by the firmware, but the host read the reply
header before the bridge had armed it. Every later command then failed
until `cc3501e_recover()`. Below and above that band the same call worked.

The cause was the host's reply-header gate. Its flat 200 us zone, proven
for reply size up to 536 B, was also applied to request size. Measured on
E1M-AEN803 2026W36-0009 at 25 MHz with PACK32 and all-PIO SPI, the bridge
needs about 54 us + 0.426 us per request byte before its reply header is
ready, so a 400 B request already needs more than 200 us. The host now
adds a request-side floor of 80 us + 1 us per 2 request bytes, capped at
2000 us, on firmware without FAST_REPLY. The existing size gate is kept as
the lower bound, so no exchange waits less than before. With the floor,
SOCK_SEND ran 345 of 345 clean from 200 to 4086 bytes on silicon, where
the stock gate failed every length from 400 to 610 bytes.
