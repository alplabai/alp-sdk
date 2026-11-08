### Fixed — orchestrate: generated `alp_storage_mounts[]` uses `PARTITION_ID`, not the deprecated `FIXED_PARTITION_ID` (#3046)

The orchestrator's mount emitter wrote `FIXED_PARTITION_ID(<name>_partition)` for littlefs, fat and ext4 mounts. Zephyr v4.4.1 marks that macro deprecated, so any board.yaml with a storage mount failed under `-Werror`. The generated table, the Kconfig hint comment and the docs now use `PARTITION_ID(<name>_partition)`, which takes the same node-label argument.
