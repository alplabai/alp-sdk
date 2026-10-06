### Fixed — reading a GD32 bridge GPIO line no longer biases the pad (#2701)

Reading an EVK net such as `SDIO_MUX_EN` (IO29, GD32 PD11) through sysfs
drove it high and disconnected the microSD. The cause is in
`gd32-bridge-firmware`, not in the Linux `gd32-bridge-gpio` driver: the
driver's `.get()`/`.get_multiple()` only issue `CMD_GPIO_READ` and never
call `direction_input`, but the firmware's first `CMD_GPIO_READ` of a parked
pad enabled INPUT + PULLUP.

- The firmware now promotes a parked pad to a floating input (no pull) on
  read, so a read samples the net without changing its level
  (`alplabai/gd32-bridge-firmware`, `hal/gd32/gpio.c`, commit `ed36b62`,
  first released after `v0.3.0`). Bridge firmware newer than `v0.3.0`
  that carries `ed36b62` is required; no kernel patch change is needed.
- Host-visible change: an unconnected or undriven E1M IO used to read a
  stable 1 after its first read (internal 40 kΩ pull-up). It now floats and
  reads an indeterminate level unless the carrier pulls it.
- Unchanged: `CMD_GPIO_WRITE` still promotes a pad to push-pull output until
  GD32 reset and the wire protocol has no demote opcode, so
  `direction_input()` on a line that has been written keeps returning
  `-EPERM`. Lifting that needs a protocol addition (a demote opcode).
