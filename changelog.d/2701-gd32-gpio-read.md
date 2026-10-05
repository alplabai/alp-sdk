### Fixed — reading a GD32 bridge GPIO line no longer biases the pad (#2701)

Reading an EVK net such as `SDIO_MUX_EN` (IO29, GD32 PD11) through sysfs
drove it high and disconnected the microSD. The cause is in
`gd32-bridge-firmware`, not in the Linux `gd32-bridge-gpio` driver: the
driver's `.get()`/`.get_multiple()` only issue `CMD_GPIO_READ` and never
call `direction_input`, but the firmware's first `CMD_GPIO_READ` of a parked
pad enabled INPUT + PULLUP.

- The firmware now promotes a parked pad to a floating input (no pull) on
  read, so a read samples the net without changing its level
  (`alplabai/gd32-bridge-firmware`, `hal/gd32/gpio.c`). Bridge firmware
  that carries the fix is required; no kernel patch change is needed.
- Unchanged: `CMD_GPIO_WRITE` still promotes a pad to push-pull output until
  GD32 reset and the wire protocol has no demote opcode, so
  `direction_input()` on a line that has been written keeps returning
  `-EPERM`. Lifting that needs a protocol addition.
