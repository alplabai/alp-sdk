### Changed — `ota:` now requires `offset_kib:` on every `storage:` partition (#2988)

**When `ota.provider` is set (not `none`), the board.yaml validator refuses any `storage[]` entry (SoM `memory_map:` regions excluded) without `offset_kib:` and names the unpinned partitions.** Unpinned partitions are allocated in name order, so adding or renaming one silently moved later OTA slots; pins never move. `examples/connectivity/production-deployment/board.yaml` now pins its layout, and `board.schema.json` plus the OTA docs state the rule.
