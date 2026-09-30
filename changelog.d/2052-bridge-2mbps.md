### Changed — CC3501E bridge reaches 2 MB/s: settles counted from the last transfer, the CRC hidden inside them, and shorter gates for fast firmware (#2052)

On E1M-AEN803 2026W36-0009, 4092-byte `STREAM_WRITE` frames went from
1.27 MB/s to about 2.01 MB/s against cc3501e-bridge-firmware #155 (TCM
per-frame path, slicing-by-4 request CRC, SPI driver in code TCM).

- **New `alp_uptime_us()`.** It is a microsecond counterpart of
  `alp_uptime_ms()`: cycle-counter backed on Zephyr where a 64-bit cycle
  counter exists (AEN), `CLOCK_MONOTONIC` on Linux, and the stub's
  microsecond clock elsewhere.
- **Inter-phase settles count from the end of the previous transfer**, not
  from the moment the gate is called. `cc3501e_t` records
  `last_xfer_end_us`, so host work done between two phases counts toward the
  settle instead of adding to it.
- **The request CRC is split around the header.** The header and the first
  half of the payload are CRC'd before the request header, and the rest
  after it, so most of the ~150 µs CRC runs inside the two settles.
- **The shared CRC-16 uses slicing-by-4 with one 32-bit load per step.**
  About 150 µs per 4 KiB frame on the M55-HE (179 µs single-table,
  ~280 µs before that).
- **Firmware reporting `ALP_CC3501E_CAP_FAST_REPLY` gets shorter waits:**
  a 50 µs phase settle (was 250 µs), and a reply gate rising from 200 µs at
  536 B to 450 µs at 4092 B (was 800 µs). Measured need: 10 to 25 µs for
  the settle, and 300 to 400 µs for the reply at 4092 B. Firmware without
  the capability keeps 250 µs and the 2048 B / 2000 µs plateau.

Per-frame breakdown at 4092 B (µs): settle and first half-CRC 77, header 11,
second half-CRC plus copy 15, payload 1317 (25 MHz wire), reply gate 450,
reply header 11, settle 50, reply 14.
