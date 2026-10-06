### Added — GD32 bridge protocol v0.17 host mirrors: I2C3 master proxy opcodes and CAM_EN_LDO GPIO bits (alplabai/gd32-bridge-firmware#298)

`gd32-bridge-firmware` protocol v0.17 lets Linux use the bridge as the master
of E1M-X I2C3 and adds the four SoM camera LDO enables to the GPIO expander.
This change lands the host-side header, wire doc and board metadata.

Changes:
- `<alp/chips/gd32g553.h>` gains `GD32G553_CMD_I2CM_CONFIG` / `_XFER` /
  `_RESULT` (`0xA0..0xA2`), `gd32g553_i2cm_result_t`,
  `GD32G553_I2CM_MIN_PROTOCOL_MINOR` (17) and
  `GD32G553_GPIO_LINE_CAM_EN_LDO0..3` (bridge bits 23..26, `PC3` / `PE8` /
  `PE7` / `PE10`).  No host helper is added: the opcodes are I2C-link only and
  the CM33 has no caller.
- `docs/gd32-bridge-protocol.md` documents the opcodes (new section 3.20), the
  result codes, the I2C allow-list entry and the v0.17 history row.
- `docs/boards/e1m-x-evk.md` and `metadata/boards/e1m-x-evk.yaml` say I2C3 is
  now served by the bridge proxy, and that the X-EVK V2 J6 display I2C is not
  wired to E1M-X I2C3 on the carrier netlist (J12 / CAM1 is).
- `metadata/chips/gd32g553.yaml` records the I2C3 pads and the proxy.
