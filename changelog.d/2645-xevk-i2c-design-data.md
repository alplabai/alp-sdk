### Fixed — X-EVK V2: PCIe/M.2 I2C switch enable is active-low; mikroBUS and display I2C carrier gaps documented (#2645)

- `XEVK_PIN_PCIE0_I2C_EN` (E1M-X IO2) drives the enable of a TI TMUX121, whose EN input is active-low (datasheet pin 8). The net has no pull resistor. The route doc said "drive high to enable", which disconnected the M.2 slots. It now says to drive the pin low (`metadata/boards/e1m-x-evk.yaml`, `alp_e1m_x_evk_routes.h`).
- `docs/boards/e1m-x-evk.md` states that on the X-EVK V2 the mikroBUS socket I2C and the J6 display I2C each sit behind a level shifter whose module side is not wired to any E1M-X I2C bus. Touch and panel-bridge parts on J6, and Click boards on mikroBUS, are not reachable from the SoM without a carrier fix. J6 touch INT and RST are wired through.
