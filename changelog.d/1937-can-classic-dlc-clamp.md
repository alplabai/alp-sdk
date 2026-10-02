### Fixed — CAST CAN-FD driver delivers a classic DLC 9..15 frame as 12..64 bytes (#1937)

On a `CONFIG_CAN_FD_MODE` build, `can_cast_receive()` reported a received
classic frame (FDF=0) with wire DLC 9..15 as `can_dlc_to_bytes(dlc)` =
12/24/64 bytes, copying stale receive-buffer words beyond the 8 data bytes
classic CAN carries (measured on E1M-AEN803 in internal loopback: DLC 9 gave
`payload_len` 12 with `data[8..11]` stale). Per ISO 11898-1 such a frame
carries 8 bytes.

Changes:
- `zephyr/drivers/can/can_cast.c`: a classic frame with DLC > 8 is reported
  with `dlc = 8` and only 8 bytes are copied, on FD and non-FD builds alike.
  An FD frame on a non-FD build is still dropped (no room in `can_frame`).
- `src/backends/can/zephyr_drv.c`: the RX trampoline caps a classic frame's
  `payload_len` at 8 so drivers that pass the raw DLC through agree.
