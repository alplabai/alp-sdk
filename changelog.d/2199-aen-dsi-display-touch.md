### Added — the RK055HDMIPI4MA0's GT911 capacitive touch controller now works on the E1M-EVK (#2199)

The panel's touch controller is a Goodix GT911 on the flex, on the shield's
existing DSI touch I2C bus (`&i2c1`). Its reset is expander U35 P3 (net
`CTP_RST`); its INT line (`CTP_INT_L` on `J6` pin 29) runs through level
shifter U4 to E1M module connector pad `AH8`, which the 2626-R2 netlist and
`metadata/pinmux/aen.yaml` assign to the CC3501E Wi-Fi/BLE coprocessor
(`silicon_pad: GPIO_15`), not to the Alif SoC -- the Alif can never see this
INT line on this board revision, and `U35` has no spare pin to route it
anywhere else.

Upstream's `drivers/input/input_gt911.c` already supports polling
(`CONFIG_INPUT_GT911_INTERRUPT` off by default) and documents `alt-addr` as
"useful for boards that do not route the INT pin" -- but the driver
contradicted its own binding: `dts/bindings/input/goodix,gt911.yaml` marks
neither `irq-gpios` nor `reset-gpios` as required, yet the driver built
`int_gpio` with the plain (non-`_OR`) `GPIO_DT_SPEC_INST_GET()` and
dereferenced it unconditionally in `gt911_init()` and in
`gt911_pm_state_exit()`. Any board with no `irq-gpios` faulted on a zeroed
`gpio_dt_spec` before it ever reached `alt-addr` probing.
`zephyr/patches/zephyr/0004-input-gt911-fall-back-to-polling-without-an-irq.patch`
makes `irq-gpios` genuinely optional -- mirroring how `reset-gpios` was
already handled -- and turns a still-required INT pin under
`CONFIG_INPUT_GT911_INTERRUPT` into a clean `-ENODEV` instead of a fault.

The `e1m_evk_rk055hdmipi4ma0` shield's new GT911 node sets both `reg = <0x5d>`
(the address the GT911 forces when INT is held low across reset) and
`alt-addr = <0x14>` (its power-on default) -- both matter, in either state of
this board's CC3501E. As shipped, this EVK's CC3501E is not flashed, so
`GPIO_15` never drives `CTP_INT`, the net floats at GT911 reset, and the
controller is bench-confirmed responding at the alternate address:
`touch : RESPONDING at 0x14 (alternate) product-id[0x8140] rc=0
data=39 31 31 00` ("911" in ASCII). If the CC3501E is later flashed with
firmware that drives `GPIO_15`, the level `CTP_INT` sees at reset can change
and the controller can select `0x5D` instead; the patched driver's `alt-addr`
probing covers both outcomes without a board-specific edit.
