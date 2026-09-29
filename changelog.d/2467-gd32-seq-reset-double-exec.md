### Fixed — GD32 host driver no longer double-executes commands after a bridge reset (alplabai/alp-sdk#2467)

After a GD32 reset (OTA commit/rollback, watchdog) the firmware's
`STATUS_SEQ` link feature is off and every reply is stamped 0, but the host
kept `seq_enabled`/`seq_last`. The stamp-0 replies then read as stale, the
request was re-sent although the bridge had already executed it (twice for a
non-idempotent opcode), and the call failed `ALP_ERR_IO`. The "a stale verdict
proves the request was never executed" comment was false across a reset.

Changes:
- `chips/gd32g553` classifies each reply stamp as fresh / stale / reset. A
  reset signature (stamp 0 after a non-zero baseline, or a stamp-0 "stale")
  never re-sends: the driver drops its sequencing state, re-negotiates
  `CMD_LINK_FEATURES` and fails that one call with `ALP_ERR_IO`. The legitimate
  `0xF` -> `0` stamp wrap is still accepted.
- `gd32g553_ota_commit()` / `gd32g553_ota_rollback()` drop the sequencing
  state on success, since both reset the bridge.
- `docs/gd32-bridge-protocol.md` 4.1.1 corrected. Wire protocol and
  `PROTOCOL_VERSION` are unchanged.
- New `tests/unit/gd32_link_seq_reset` simulates a bridge reset at the SPI
  byte level and pins that no request executes twice.
