### Fixed — CAST CAN-FD driver defects found by internal loopback on E1M-AEN803 (#914)

Running CAN-FD internal loopback (`LBMI`) on E1M-AEN803 (serial 2026W36-0001)
showed the shipped `zephyr/drivers/can/can_cast.c` queued frames but never
transmitted them. Fixes:

- **Missing CGU clock enable.** `can_cast_init()` now sets `CGU_CLK_ENA`
  (`0x1A602014`) bit 23 as well as bit 20, as the Alif Zephyr fork does for an
  okay can0. Without it nothing left the transmit buffer (`0xfe33fff1` cold vs
  `0xfeb3fff1` working). Bit 23 is named neutrally: the fork calls it HFOSC,
  `dphy_dw.c` CLK38P4M.
- **Struct layout.** `common` is now the first member of `struct can_cast_config`
  and `struct can_cast_data` (Zephyr's `can.h` requires it); before,
  `can_get_bitrate_max()` returned 0 and `can_set_bitrate()` /
  `can_set_bitrate_data()` returned -134.
- **Timing dropped after `can_set_mode()`.** `can_set_timing()` and
  `can_set_timing_data()` now hold the core in reset around the register write
  and restore the previous reset state, so the standard order
  (`set_timing` -> `set_mode` -> `start`) and the reverse both apply.
- **`can_get_core_clock()`** reports `CANFD_CTRL` source / `CKDIV` (Alif DFP
  `sys_ctrl_canfd.h` / `Driver_CAN.c`) instead of the 200 MHz clockctrl parent;
  falls back to the old path if `CKDIV` < 2. Bench `CANFD_CTRL = 0x00111010`
  is 160 MHz / 16 = 10 MHz.
- **Lost TX-done callbacks.** The IRQ now clears TSIF first, then completes every
  queued frame the transmit buffer no longer holds (200 frames gave 23 callbacks).
- **RTR livelock with `CONFIG_CAN_ACCEPT_RTR=n`.** The early return now releases
  the RX buffer; before, IRQ 104 retriggered forever and froze the M55-HE.
- **Smaller defects:** `can_cast_stb_single_shot_mode()` masked with the bit
  index (`3`) instead of `BIT(3)`; `get_max_filters` and `remove_rx_filter` use
  `CONFIG_CAN_MAX_FILTER` instead of 16 (out-of-bounds write);
  `can_cast_enable_tx_interrupts()` `&= BIT(TPIE)` is now `&= ~BIT(TPIE)` so it
  stops clearing the other RTIE bits; the send timeout countdown saturates at 0
  instead of wrapping for timeouts that are not a multiple of 100 us.
