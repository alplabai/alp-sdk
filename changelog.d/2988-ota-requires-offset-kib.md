### Changed — `ota:` now requires `offset_kib:` on every `storage:` partition (#2988)

**When a project declares `ota:`, the board.yaml validator refuses any `storage[]` entry without `offset_kib:` and names the unpinned partitions.** Unpinned partitions are allocated in name order, so adding or renaming one silently moved later OTA slots; pins never move. `examples/connectivity/production-deployment/board.yaml` now pins its layout, and `board.schema.json` plus the OTA docs state the rule.
