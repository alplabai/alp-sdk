### Fixed — CC3501E bridge: 2048-byte frames get the full reply-header gate, and the throughput example packs its SPI FIFO (#2052)

The host waits a blind, size-scaled gap before reading a reply header,
because READY is not readable on this carrier. The gap was interpolated
linearly between 200 us at 536 B and 2000 us at 4092 B, which gave about
970 us at 2048 B. Once the host got faster, with `CONFIG_SPI_DW_ALIF_PACK32`
or with the D-cache on, that was too short. On E1M-AEN803 2026W36-0009,
2048-byte `STREAM_WRITE` accounted for 4 of the 6 `rc=-5` failures seen on
2026-09-28. The link-failure ring showed a reply-header read of all zeros,
meaning the slave had not re-armed yet, and every retry after it failed too.
The plateau now starts at 2048 B. Below that the gate still interpolates
from the 536 B floor.

`aen-cc3501e-socket-throughput` now sets `CONFIG_SPI_DW_ALIF_PACK32=y`, as
`aen-cc3501e-bringup` already does (#1725). Measured on the same unit in
the app's own configuration (D-cache off, M55-HE Flow C RAM-run), bridge
sweep:

| gate plateau | runs clean | 1024 B | 2048 B | 4092 B |
|---|---|---|---|---|
| old (4092 B), PACK32 | 1 of 2 | 274 KB/s | 316 KB/s (failed `-5` in the other run) | 342 KB/s |
| 2048 B, PACK32 | 3 of 3 | 251 KB/s | 271 KB/s | 342 KB/s |

A plateau from 1024 B was also 3 of 3 clean, but it cut 1024-byte frames to
192 KB/s, so it was not taken. Without PACK32 the same app measured
269 KB/s at 4092 B.

The host-driver ztests move the interpolation midpoint to 1292 B and pin
2048 B to the 2000 us plateau.
