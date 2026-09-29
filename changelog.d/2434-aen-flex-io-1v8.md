### Fixed — E1M-AEN flex-IO pins now run in 1.8 V mode (#2434)

The Alif E8 flex-IO bank (GPIO7 pins 4..7 and the LPGPIO flex pins) takes its
level mode from VBAT `GPIO_CTRL` bit 0 (`0x1A609000`), which resets to 3.3 V
mode. The E1M-AEN SoM supplies `VDD_IO_FLEX` at 1.8 V, and nothing set the
bit, so a flex pin's input threshold sat above its own rail.

A new `CONFIG_ALIF_FLEX_IO_1V8` (in `zephyr/Kconfig`) sets the bit at
`PRE_KERNEL_1` from `zephyr/soc-bridge/alif/flex_io_1v8_e8.c`, matching what
the Alif DFP's `board_pins_config()` does. It defaults on for the four E8
AEN boards (`e1m_aen801_m55_he`, `e1m_aen801_m55_hp`, `e1m_aen803_m55_he`,
`e1m_aen803_m55_hp`) and is visible outside `CONFIG_ALP_SDK`, so MCUboot
images get it too. Only enable it on a board whose `VDD_IO_FLEX` is a 1.8 V
rail.

Verified on E1M-AEN803 2026W36-0009: `GPIO_CTRL` reads `0x00000000` after a
cold cycle and `0x00000001` once an image built with the fix boots.
