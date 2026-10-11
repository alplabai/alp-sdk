### Fixed — Linux can-stby keeps its own minor-13 gate (#2725)

The Linux `gpio-gd32-bridge` `can-stby` line (bridge bit 20) is gated on protocol minor 13 again, through its own status cache (new kernel patch 0032, follow-up to #2725), instead of sharing the IO15/IO26 cache that #2739 moved to minor 16. Without this a 0.13..0.15 bridge lost CAN standby on Linux. The userspace `chips/gd32g553` table already gated `CAN_STBY` at 13. `docs/gd32-bridge-protocol.md` now says the IO15/IO26 pad map starts at minor 16.
