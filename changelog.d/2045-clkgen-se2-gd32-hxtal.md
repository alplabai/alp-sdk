### Fixed — U-Boot routes the on-module clock generator's SE2 from DIV4 so the GD32 HXTAL gets 24.576 MHz (#2045)

SE2 of the on-module 5L35023B (BRD_I2C, 7-bit `0x69`) drives the GD32
supervisor's OSCIN (net `GD32_OSC`). The as-shipped OTP has register `0x1F`
= `0x46`; its bit 7 `SE2_Freerun_32K` = 0 makes SE2 free-run at 32.768 kHz,
which is why the GD32 HXTAL never starts.

U-Boot patch `0013-rzv2n-dev-ALP-E1M-clkgen-se2-gd32-hxtal.patch` extends
`alp_clk5l_fixup()` with two more volatile register writes (never OTP; register
`0x00` is never written), in this order, each read back and skipped when
already correct:

* register `0x24` `0x9c` -> `0x8f`: the existing `0x8e` plus bit 0
  `DIV4_CH2_EN`, so DIV4 channel 2 is live before SE2 selects it.
* register `0x1F` `0x46` -> `0xc7`: sets bit 7 `SE2_Freerun_32K` and bit 0
  `SE2_CLKSEL1` (DIV4, was DIV5); `VDD2_SEL` is unchanged.

SE2 is then DIV4 = 24.576 MHz, the same source SE3 already uses. U-Boot logs
`ALP: 5L35023B clock: SE1 32.768 kHz, SE2 24.576 MHz, SE3 24.576 MHz
(0x24=0x8f 0x1f=0xc7 0x21=0xc0)`. SE2 must not change afterwards: the GD32
locks its PLL to it once the host triggers the switch. The `clkgen_verify`
provisioning step now expects `0x1F` = `0xC7` and `0x24` = `0x8F` and reports
`SE2 24.576 MHz`. The GD32 firmware's IRC8M `SystemInit()` override is
unchanged. The next build's corrected OTP image (ledger item 24) should carry
`0x1F` = `0xC7` and `0x24` = `0x8F` so SE2 is valid from power-on-reset.

Not yet bench-verified: scope TP88 should read 24.576 MHz at 1.8 V.
