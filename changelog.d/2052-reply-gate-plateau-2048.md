### Changed — CC3501E bridge reaches 1.27 MB/s: faster host path, a SPI FIFO fix, and a shorter reply gate for fast firmware (#2052)

On E1M-AEN803 2026W36-0009, 4092-byte `STREAM_WRITE` frames went from
561 KB/s to 1,276,266-1,277,817 B/s (3 of 3 clean sweeps). That needs this
change plus cc3501e-bridge-firmware `perf/bridge-hot-path-ram`.

Host side, per 4092-byte frame:

- **`spi_dw_alif`: the polled loops no longer overflow the RX FIFO.** The
  refill budget counted the TX and RX FIFO levels but not the frame sitting
  in the shift register. A loop fast enough to keep the TX FIFO full (seen
  at `-O2`) then had one frame more in flight than the RX FIFO holds. RX
  never completed, and the transfer ended in `packed transfer stalled` /
  `rc=-4`.
- The request payload is clocked TX-only (`rx = NULL`), because nothing
  reads the slave's dummy bytes. Without RX drains the polled loop runs at
  wire speed: 1.31 ms for 4094 B at 25 MHz, down from 1.91 ms.
- Only the bytes each read phase clocks are cleared. A `memset` of the whole
  4100-byte scratch buffer cost up to 165 µs per request under `-Os`.
- The shared CRC-16 uses a lookup table (see the #1677 CRC fragment).

Reply-header gate:

- Old firmware: the 2000 µs plateau now starts at 2048 B. Interpolated,
  2048 B only got about 970 µs, and 2048-byte frames caused 4 of the 6
  `rc=-5` wedges seen on 2026-09-28.
- New firmware: firmware that reports the new
  `ALP_CC3501E_CAP_FAST_REPLY` capability (bit `0x00001000`) runs its
  per-frame path from RAM and arms the reply for a 4 KiB frame in 300 to
  600 µs. `cc3501e_reset()` reads the capability, and the gate then rises
  from 200 µs at 536 B to 800 µs at 4092 B, about 1.8× the measured need.
  Firmware without the bit keeps the conservative gate.
- The flag is stored in `cc3501e_t::fw_fast_reply`, in padding, so the
  struct layout does not move.

`aen-cc3501e-socket-throughput` now builds the way a product runs:
`CONFIG_DCACHE=y` (the board default), `CONFIG_SPEED_OPTIMIZATIONS=y` and
`CONFIG_SPI_DW_ALIF_PACK32=y`. With the D-cache off, the same link measures
~563 KB/s, because the host becomes CPU-bound. `aen-bench-shared.conf` still
forces the cache off, so a Flow C measurement must pass `-DCONFIG_DCACHE=y`.

New host-driver ztests pin the 2048 B plateau and the fast-reply table.
All nine CC3501E suites pass (535 cases).
