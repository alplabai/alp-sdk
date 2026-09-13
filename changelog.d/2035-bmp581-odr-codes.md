### Fixed — five of `bmp581_odr_t`'s seven ODR codes selected the wrong rate (#2035)

`include/alp/chips/bmp581.h`'s `bmp581_odr_t` picked its seven codes as
if `ODR_CONFIG`'s 5-bit field were a linear divider; BST-BMP581-DS004-13
§7.34 (p.65)'s ODR table is not. `BMP581_ODR_5_HZ` (0x17) actually
selected 10.000 Hz, `BMP581_ODR_10_HZ` (0x14) actually selected 25.005 Hz,
`BMP581_ODR_25_HZ` (0x0E) actually selected 60.000 Hz, `BMP581_ODR_50_HZ`
(0x07) actually selected 129.855 Hz, and `BMP581_ODR_120_HZ` (0x01)
actually selected 218.537 Hz -- `BMP581_ODR_240_HZ` (0x00) and
`BMP581_ODR_1_HZ` (0x1C) were already correct. Corrected to 0x00, 0x08,
0x0F, 0x14, 0x17, 0x18, 0x1C respectively, cross-checked against Bosch's
own BMP5-Sensor-API `bmp5_defs.h` (`BMP5_ODR_*`), which is generated from
the same table. This only affects NORMAL mode continuous sampling, not a
FORCED one-shot. ABI-visible: any code that has `BMP581_ODR_5_HZ` etc.
baked into a saved config value needs to be rebuilt against this enum,
not just relinked.
