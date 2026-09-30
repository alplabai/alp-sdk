# High-score persistence: options (booth)

Status: open decision. What ships today: the table (`src/game/hiscore.h`,
top 5 + initials) lives in HE RAM only. It survives runs and walk-aways, not
a power cycle or a re-flash. Nothing writes MRAM, EEPROM or flash.

What persisting needs: about 44 B (5 x {u32 score, 3 letters}), written only
when a run enters the table. At a booth that is a few writes an hour.

| Option | Where | Risk | Cost |
|---|---|---|---|
| **A. MRAM, a dedicated sector outside every image** | on-SoC MRAM, a reserved range in the ATOC/memory map | A flash-run replaces the whole ATOC and can wipe the sector. The J-Link Flow D loader rewrites whole 16 KiB sectors, so neighbours must be sector-aligned. A power cut mid-write tears the record. | Two alternating slots, each `{magic, seq, crc32, table}`; boot takes the newest valid one. Reserve the range in the memory map. Write only on an insert. Wear is not a concern at booth rates. |
| **B. SoM EEPROM (0x50 / 0x58)** | carrier I2C | **Do not use.** It holds the board's identity data; one wrong address or page overwrites it, and its secure page cannot be undone. | none |
| **C. OSPI NOR on the carrier** | OSPI0 | Shares the bus with HyperRAM (bus hazards). The chip is not fitted on every carrier, and it needs a new driver path. | Driver, erase/program plus the same two-slot record. |
| **D. microSD** | SDIO mux on the carrier | The 2626-R2 SD/I2S mux rework history makes it risky; unreworked carriers must not drive the mux. The card can be pulled mid-write. | FS stack and mount handling. |
| **E. Host over UART/USB** | booth PC | No board storage risk. Needs a PC attached and a small protocol. | Host tool. |

Recommendation: **A**, with two alternating CRC-checked slots, written on
insert only and read once at boot. Adopt it only after reserving the range
in the MRAM map and checking it against the release flow (`a32/release/`).
