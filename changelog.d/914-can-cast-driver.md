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
  (`set_timing` -> `set_mode` -> `start`) applies. The reset pulse clears
  `CFG_STAT.LBMI` (and listen-only / one-shot), so the mode bits are re-applied
  after it (shared `can_cast_apply_mode_bits()` with `set_mode`), which keeps
  the reverse order working. `can_send()` now returns `-ENETDOWN` if the core is
  held in reset and `-EIO` if loopback was requested but `LBMI` is clear,
  instead of returning 0 and silently dropping the frame.
- **Core clock now follows the DT `clock-frequency`.** `can_cast_init()` programs
  `CANFD_CTRL.CKDIV` (bits [7:0]) as source / `clock-frequency` (160 MHz / 20 MHz
  = 8; Alif DFP `sys_ctrl_canfd.h` / `Driver_CAN.c`), where the clockctrl node
  only left the reset value 0x10 (10 MHz, which made 2 Mbit/s data phase
  unreachable: `can_set_bitrate_data(2000000)` returned -ERANGE).
- **Data-phase timing limits** match the DFP (`Driver_CAN.c`: seg1 2..0x11,
  seg2 1..8, prescaler 1..4): `can_set_timing_data()` accepted prescaler <= 2
  only, and `CAN_MIN_BIT_TIME_DATA` required `phase_seg2` >= 2.
- **`can_get_core_clock()`** reports `CANFD_CTRL` source / `CKDIV` (Alif DFP
  `sys_ctrl_canfd.h` / `Driver_CAN.c`) instead of the 200 MHz clockctrl parent;
  falls back to the old path if `CKDIV` < 2.
- **Lost TX-done callbacks.** The IRQ now clears TSIF first, then completes every
  queued frame the transmit buffer no longer holds (200 frames gave 23 callbacks).
- **RX-buffer livelock on dropped frames.** The early return for an RTR frame
  with `CONFIG_CAN_ACCEPT_RTR=n`, and for a frame whose DLC exceeds
  `CAN_MAX_DLEN` (an FD frame on a `CONFIG_CAN_FD_MODE=n` build), now releases
  the receive buffer; before, IRQ 104 retriggered forever and froze the M55-HE.
- **Lost last callback.** `can_send()` now publishes the callback queue head under
  `irq_lock()` before it starts the transmit, so a TSIF IRQ in between no longer
  sees an empty queue; the head/tail reset in `can_stop()` is locked too.
- **Full callback queue.** 16 queued callbacks read as an empty queue
  (`head == tail`); the queue now holds 15 and `can_send()` returns `-EAGAIN`
  beyond that.
- **Reset pulse.** Measured by register readback on E1M-AEN803, `CFG_STAT.RESET`
  clears only `LBMI`; `TCTRL`, the timing/TDC registers, `ACFCTRL`, `ACF_EN` and the
  filter code/mask registers survive it. `can_cast_reapply_after_reset()` therefore
  only re-applies the mode bits.
- **RX filters ignored by hardware.** The acceptance-filter code/mask registers
  ignore writes unless `CFG_STAT.RESET` is set (the DFP configures filters only in
  INIT mode, `Driver_CAN.c`), so a filter added after `can_set_mode()`, or replaced
  while started, kept its reset defaults and only the ISR software filter ran.
  `can_add_rx_filter()` now holds reset around the write and re-applies the mode
  bits. While started this briefly takes the node off the bus.
- **`can_stop()` then `can_start()` lost the mode.** `can_stop()` cleared the stored
  mode and the reset dropped `LBMI`, so after a restart classic frames went to the
  pins without a completion and FD frames returned `-EINVAL`. The mode now only
  changes in `can_set_mode()`; `can_start()` re-applies the mode bits and the FD
  enable.
- **Known deviation: error counters.** Zephyr wants `can_start()` to reset TEC/REC.
  The core has no control for that (the DFP reads `TECNT`/`RECNT` only, and a reset
  pulse did not clear `TECNT`), so they carry over a stop/start.
- **`can_send()` guards.** Argument checks (`-EINVAL`) run before state checks,
  `CFG_STAT` is read once, and a lost `LBMI` is logged as an internal fault
  (`-EIO`).
- **Arbitration loss.** The handler no longer dereferences a NULL callback and
  only completes the queue entry (callback with error 1, tail popped) in
  single-shot mode; otherwise the hardware retries and `complete_tx` reports
  success once.
- **CGU enable ordering and clock rounding.** The `CGU_CLK_ENA` bits are set
  before `CANFD_CTRL` is written, and a `clock-frequency` that does not divide
  the 160 MHz source logs a warning.
- **Smaller defects:** `can_cast_stb_single_shot_mode()` masked with the bit
  index (`3`) instead of `BIT(3)`; `get_max_filters` and `remove_rx_filter` use
  `CONFIG_CAN_MAX_FILTER` instead of 16 (out-of-bounds write);
  `can_cast_enable_tx_interrupts()` `&= BIT(TPIE)` is now `&= ~BIT(TPIE)` so it
  stops clearing the other RTIE bits; the send timeout countdown saturates at 0
  instead of wrapping for timeouts that are not a multiple of 100 us.

Bench, E1M-AEN803 serial 2026W36-0001, internal loopback after round two: every
frame type 100/100, burst TX callbacks 200/200, core clock 20 MHz (`CKDIV` 8),
2 Mbit/s data phase at 10 tq, `LBMI` kept when timing is set after `set_mode`,
and no livelock with `CONFIG_CAN_ACCEPT_RTR=n`.
