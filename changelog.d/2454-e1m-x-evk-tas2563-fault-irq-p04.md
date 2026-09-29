### Fixed -- E1M-X EVK TAS2563 AMP.FAULT interrupt now uses P04 (#2454)

`e1m-x-evk.dtsi` wired the shared AMP.FAULT (IRQ_N) interrupt to `RZV2N_GPIO(1, 5)` (P15, E1M `I2S1_SDO`). The carrier routes it to E1M `I2S1_SDI`, which the SoM routes to RZ/V2N `P04` (pad `AR10`), so it is now `RZV2N_GPIO(0, 4)`. No pinctrl group claims P04 or P15.
