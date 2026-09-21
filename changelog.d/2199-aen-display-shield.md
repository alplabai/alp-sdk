### Added — any app on an E1M-AEN SoM can drive the E1M-EVK's RK055HDMIPI4MA0 panel with one shield (#2199)

The Alif E8 display chain (CDC200 -> DesignWare MIPI-DSI host -> D-PHY -> the
upstream `himax,hx8394` driver) used to exist only inside
`examples/aen/aen-dsi-display`: its overlay defined the SoC nodes, and its
`src/main.c` did the SoC clock/power setup. It is now split so an app gets the
whole chain by adding a shield; the app must also set `CONFIG_ALP_SDK=y`, as
every alp-sdk AEN app does, because the Alif display-driver symbols live under
`if ALP_SDK` (the shield does not turn the SDK on):

- **SoC nodes.** `zephyr/dts/alif/ensemble_e8_peripherals.dtsi` declares
  `cdc200: cdc200@49031000` (`tes,cdc-2.1`) and `mipi_dsi: mipi-dsi@49032000`
  (`snps,designware-dsi`), `status = "disabled"`, with SoC facts only: reg, the
  ten CDC IRQs 333..342 and DSI IRQ 343 with their names, the re-authored
  `ALIF_DPI_CLK` / `ALIF_DSI_CLK` / `ALIF_CDC200_PIX_SYST_ACLK` clocks,
  `cdc-if = <&cdc200>`, `phy-if = <&dphy 1>`, `dpi-shutdn-active`,
  `dpi-colorm-active`, `ecc-recv-en` and `crc-recv-en`. No `frame-ack-en` and no
  panel timings, layers or framebuffer. The `tes,cdc-2.1` timing properties stay
  `required:`; edtlib enforces that only on `okay` nodes.
- **D-PHY clocks.** The shared `&dphy` node's `pllref-clk` / `pllbypass-clk` /
  `tx-dphy-clk` are now the real `ALIF_MIPI_PLLREF_CLK` / `ALIF_MIPI_BYPASS_CLK` /
  `ALIF_MIPI_TXDPHY_CLK` (`MIPI_CKEN` bits 8 / 12 / 0) instead of the
  `ALIF_CSI_DPHY_CLK` placeholder, which made `dphy_dw_master_setup()` stall the
  AXI on unclocked config registers. `rx-dphy-clk` (camera RX) stays on the
  placeholder. This applies to every build with `dphy` okay, not only the
  display: `dphy_dw_init()` now turns on the real PLL-reference gate
  (`MIPI_CKEN` bit 8) at boot, and `dphy_dw_slave_setup()` the real bypass gate
  (bit 12) for a D-PHY id other than 0 -- including `aen-camera-regcheck` and
  `aen-dsi-regcheck`.
- **SoC glue.** `zephyr/soc-bridge/alif/mipi_display_e8.c`
  (ADR-0017-ADJACENT: a register sequence from the Alif DFP / DevKit, not a HAL
  consumer), built on E8 with `CONFIG_MIPI_DPHY_DW`, `CONFIG_MIPI_DSI_DW` or
  `CONFIG_DISPLAY_CDC200`, does what no driver does, each hook gated on its DT
  node:
  - at `PRE_KERNEL_1`, when `dphy` is okay, it enables the D-PHY clock sources
    (CGU `CLK_ENA` +0x14 bits 21 and 23: HFOSC 38.4 MHz and the 100 MHz CFG
    clock) and D-PHY analog power (VBAT `PWR_CTRL` +0x08: clears bits
    0/1/4/5/8/9/12). A CSI-camera build gets this too;
  - at `POST_KERNEL` `CONFIG_KERNEL_INIT_PRIORITY_DEFAULT`, BUILD_ASSERTed below
    both `CONFIG_DISPLAY_INIT_PRIORITY` and `CONFIG_MIPI_DSI_INIT_PRIORITY`, it
    sets `CDC200_PIXCLK_CTRL` (`CLKCTL_PER_MST` +0x04) bits [24:16] to
    `SYST_ACLK / cdc200 clock-frequency`, keeping the other bits. A
    `clock-frequency` that is not SYST_ACLK over an integer divider in 2..511 is
    a build error, since the DSI host times its lanes from the same property;
  - at `PRE_KERNEL_1`, with the DSI host driver built, it muxes the pad of each
    DSI panel's `bl-gpios` to GPIO with the input receiver on, when that GPIO is
    on a main-domain `snps,designware-gpio` port (gpio0..gpio14); other
    controllers are skipped.

  The CGU, VBAT and `CLKCTL_PER_MST` bases come from the clock controller's
  `reg-names`, the 400 MHz from the `syst_aclk` fixed clock. The pad mux lives
  here because `display_cdc200.c` does not apply its `pinctrl-0` in a MIPI-DSI
  build. The Secure Enclave's aiPM run profile (`run_profile_t.phy_pwr_gating` /
  `ip_clock_gating` in hal_alif `aipm.h`) also manages the D-PHY power and the
  CDC200 / DSI clocks; the glue bypasses it, so a `se_service_set_run_cfg()`
  whose profile lacks those bits may power them down under a running display.
  In-tree only `alp_power_profile_set(RUN)` calls it; not observed.
- **Shield.** `e1m_evk_rk055hdmipi4ma0` (`zephyr/boards/shields/`, found through
  the module's `board_root`) sets `zephyr,display` and `alp-display0` to
  `&cdc200`; adds the panel-enable `regulator-fixed` on expander P0, the
  `nxp,pca9538` expander at `0x73` on `&i2c2`, a 2 MiB `mmio-sram` framebuffer
  region at `0x02200000` (the top of the 4 MiB SRAM0), and the `himax,hx8394`
  panel (reset on expander P1, `bl-gpios` on `&gpio5 5`, 2 lanes, RGB565); and
  turns on `&dphy`, `&cdc200` (timings, `clock-frequency = <57142857>`,
  `memory-region`, layer 1), `&mipi_dsi` (`vid-pkt-size = <720>`) and `&gpio5`.
  It shrinks the SoC `sram0` (also the "SRAM0" linker region) to the bottom
  2 MiB, `reg = <0x02000000 DT_SIZE_M(2)>`, so SRAM0 data reaching the
  framebuffer is a link error rather than a silent overwrite. Its
  `Kconfig.defconfig` defaults on `DISPLAY`, `MIPI_DSI`, `HX8394`, `I2C`, `GPIO`,
  `GPIO_PCA_SERIES`, `REGULATOR`, `REGULATOR_FIXED`, and under `ALP_SDK`
  `MIPI_DPHY`, `MIPI_DPHY_DW`, `MIPI_DSI_DW` and `DISPLAY_CDC200`. No system
  heap is added: none of the chain's drivers allocates from it. The I2C2 pins,
  clock and `alp-i2c0` alias stay with the board devicetree.

`aen-dsi-display` now just appends the shield in `CMakeLists.txt`. Its overlays
shrink to the ITCM RAM-run retarget, `src/main.c` loses
`dphy_clock_power_enable()`, `cdc_pixclk_div_fixup()` and `backlight_on_early()`,
and `prj.conf` drops what the shield provides, including
`CONFIG_GPIO_PCA_SERIES_INIT_PRIORITY=60` and `CONFIG_HEAP_MEM_POOL_SIZE=4096`:
the defaults already order I2C (50), the expander (50, after its bus by
devicetree dependency), the regulator (75), the CDC and DSI host (85) and the
panel (90). The RAM-run `zephyr.bin` is 106,412 B (106,396 B before). Its
`testcase.yaml` now builds both the AEN801 and AEN803 HE targets.
`aen-dsi-regcheck` now enables the SoC nodes instead of declaring its own
copies. `aen-evk-demo` builds with or without the shield: with it, the TCAL9538
at `0x73` belongs to the GPIO driver, so its phase 4 checks for the shield's
`lcd_exp` node and skips its raw register writes (`SKIPPED`), and its phase 12
reports the display it can now open.

Build-verified on E1M-AEN801 and E1M-AEN803. On the bench (E1M-AEN803 2026W36-0009,
E1M-AEN803, 2026-09-18) the RAM-run read back the expected registers --
`0x4903F004 = 0x00070001`, CDC L1 framebuffer `0x49031134 = 0x02200000`,
backlight level 1, DSI `INT_ST1 = 0x00000000` over 100 ms of scanout; pixels on
glass are not yet observed.
