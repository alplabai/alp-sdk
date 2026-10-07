### Fixed — GD32 bridge OTA_GET_STATE decodes the failure cause (alplabai/gd32-bridge-firmware#101)

The `chips/gd32g553` OTA driver read a fixed 5-byte `CMD_OTA_GET_STATE`
reply, so every OTA failure surfaced to callers as an opaque
`GD32G553_OTA_STATE_ERROR` with no attributable cause. `gd32-bridge-firmware`
protocol v0.14 widens the reply to 6 bytes, adding an `err` byte.

Changes:
- `gd32g553_ota_get_state()` now reads 6 bytes (5 + `err`) against a peer
  advertising protocol minor >= `GD32G553_OTA_ERR_MIN_PROTOCOL_MINOR` (14),
  and keeps the old 5-byte read (with `err` forced to
  `GD32G553_OTA_ERR_NONE`) against an older bridge, matching the
  negotiated protocol minor rather than a compile-time assumption.
- New `gd32g553_ota_err_t` enum (`include/alp/chips/gd32g553.h`) mirrors
  the firmware's `gd32_bridge_ota_err_t` wire values, including the two
  new causes the firmware side added: `GD32G553_OTA_ERR_NOT_TRIAL_CAPABLE`
  and `GD32G553_OTA_ERR_META_DEMOTE_FAILED`.
- `gd32g553_ota_state_info_t` gains an `err` field.
- Extends the fake-bridge Zephyr host test with two cases pinning both
  wire shapes (pre-v0.14 5-byte and v0.14+ 6-byte).
