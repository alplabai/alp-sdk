### Fixed — `aen-dsi-display` drove the LCD control expander at the wrong address and part (#2199)

Both `aen-dsi-display` overlays defined `lcd_exp` as `gpio@20` / `nxp,pca6408`,
which targets `EVK_I2C_ADDR_TCA6408A_MAIN` in `metadata/boards/e1m-evk.yaml` -- a
part the metadata marks `assembled: false` on this EVK revision. The example never
picked up the U35 address correction from alp-sdk#1974.

`lcd_exp` is now `gpio@73`, `reg = <0x73>` (in the `e1m_evk_rk055hdmipi4ma0`
shield, which replaces the example's own overlay wiring): the assembled U35,
`EVK_I2C_ADDR_TCAL9538_MAIN`. The compatible is `nxp,pca9538`, not `nxp,pcal9538`.
Both bind `gpio_pca_series.c` under `CONFIG_GPIO_PCA_SERIES=y`, but
`pcal9538` is the TYPE_2 variant whose init also writes the Agile-IO block at
`0x40`..`0x45`, which is untested on this part. `nxp,pca9538` stays on the TYPE_0
base registers that #1974 read back at POR. The P0/P1 assignments (panel power
enable / HX8394 reset) are unchanged.

The example also stops pulsing Alif `P10_2` as an "expander reset". That pad came
from mixing up SoC ball N1 with E1M pad N1: the EVK's `IO_EXP_RST` is E1M pad N1 =
Alif `P3_6` (`metadata/pinmux/aen.yaml`), and pulsing even that line changed
nothing on 2 of 2 boards. U35 answers at POR without any reset action, so the hook
and its `&gpio10` enable are removed.
