# GD32 application bootloader

Status: **scaffold**.  The OTA opcode range `0xF0..0xFF` reserved
in [`../../../docs/gd32-bridge-protocol.md`](../../../docs/gd32-bridge-protocol.md)
§10 routes through this directory's dispatcher; every handler replies
with the standard `STATUS_NOSUPPORT` (0x06) until the real
implementation lands.

Integration detail (flash layout, commit sequence, rollback policy,
slot-jump path, threat model) is held by the maintainer and not
mirrored here.

## OTA opcode contract

| Opcode                | Code |
|-----------------------|------|
| `CMD_OTA_BEGIN`       | 0xF0 |
| `CMD_OTA_WRITE_CHUNK` | 0xF1 |
| `CMD_OTA_VERIFY`      | 0xF2 |
| `CMD_OTA_COMMIT`      | 0xF3 |
| `CMD_OTA_ROLLBACK`    | 0xF4 |
| `CMD_OTA_GET_STATE`   | 0xF5 |
| `CMD_OTA_ABORT`       | 0xF6 |

Host driver code that talks to the bridge can call these opcodes
today; the scaffold will reply `STATUS_NOSUPPORT` against any
firmware build that lacks the body.  This is the same degradation
path the protocol uses for any reserved-but-unimplemented opcode
(see `docs/gd32-bridge-protocol.md` §6).

## See also

* [`../../../docs/gd32-bridge-protocol.md`](../../../docs/gd32-bridge-protocol.md)
  §10 -- protocol-level reservation of the OTA opcode range.
* [`../../../docs/gd32-bridge.md`](../../../docs/gd32-bridge.md)
  "Flashing" -- end-user-visible upgrade paths.
* [`../../../chips/gd32_swd/`](../../../chips/gd32_swd/) --
  host-driven SWD recovery path (Path B in the protocol spec).
