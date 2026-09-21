### Added — the RK055HDMIPI4MA0's GT911 capacitive touch controller is now wired up on the E1M-EVK, polling only, NOT YET bench-verified (#2199)

The panel's touch controller is a Goodix GT911 on the flex, on the shield's
existing DSI touch I2C bus (`&i2c1`). Its reset is expander U35 P3 (net
`CTP_RST`); its INT line (`CTP_INT_L` on `J6` pin 29) runs through level
shifter U4 to E1M module connector pad `AH8`, which the 2626-R2 netlist and
`metadata/pinmux/aen.yaml` assign to the CC3501E Wi-Fi/BLE coprocessor
(`silicon_pad: GPIO_15`), not to the Alif SoC. The Alif can see this line only
through a CC3501E GPIO relay (`ALP_CC3501E_CMD_GPIO_SET_INTERRUPT` /
`EVT_GPIO_INTERRUPT`, `chips/cc3501e/cc3501e_gpio.c`) -- not wired up today,
so polling is the choice on this board revision, and `GPIO_15` must stay an
INPUT regardless: the GT911 drives its own INT pin after reset, so a CC3501E
output there would contend with it.

Upstream's `drivers/input/input_gt911.c` already supports polling
(`CONFIG_INPUT_GT911_INTERRUPT` off by default) and documents `alt-addr` as
"useful for boards that do not route the INT pin" -- but the driver
contradicted its own binding: `dts/bindings/input/goodix,gt911.yaml` marks
neither `irq-gpios` nor `reset-gpios` as required, yet the driver built
`int_gpio` with the plain (non-`_OR`) `GPIO_DT_SPEC_INST_GET()`, which expands
to an undefined devicetree macro when `irq-gpios` is absent -- any board with
no `irq-gpios` FAILED TO COMPILE, never mind run.
`zephyr/patches/zephyr/0004-input-gt911-fall-back-to-polling-without-an-irq.patch`
makes `irq-gpios` genuinely optional -- mirroring how `reset-gpios` was
already handled -- adds a `BUILD_ASSERT` that still requires `irq-gpios` under
`CONFIG_INPUT_GT911_INTERRUPT`, and turns a still-required INT pin into a
clean `-ENODEV` instead of a fault when that config is off but the pin is
somehow missing at runtime.

The `e1m_evk_rk055hdmipi4ma0` shield's new GT911 node sets both `reg = <0x5d>`
(what the GT911 forces when INT is held low across reset) and
`alt-addr = <0x14>` (what it forces when INT is high at that moment) -- which
one it actually latches depends on the INT level at the moment GT911's RSTB
releases, which this shield does not control. A raw I2C probe (not this
driver) found the controller responding at the alternate address:
`touch : RESPONDING at 0x14 (alternate) product-id[0x8140] rc=0
data=39 31 31 00` ("911" in ASCII) -- i.e. INT was sampled HIGH at that
moment. `alt-addr` probing (unconditional on `int_gpio` already in the
upstream driver) is what lets the driver find the device at whichever address
it actually latches; this patch only makes that code path reachable for a
board with no `irq-gpios`. The driver and DT node are wired up and reviewed,
but touch input itself has NOT been bench-verified through this driver --
that is still open.
