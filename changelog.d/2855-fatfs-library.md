### Added — `libraries: [fatfs]` selects ELM FatFs

`metadata/libraries/fatfs.yaml` (tier B, FatFs 0.16, BSD-1-Clause) makes the Zephyr ELM FatFs module selectable from `board.yaml` the same way `littlefs` is: it emits `CONFIG_FILE_SYSTEM=y` and `CONFIG_FAT_FILESYSTEM_ELM=y`. exFAT cards (most SDXC above 32 GB) still need `CONFIG_FS_FATFS_EXFAT=y` and `CONFIG_FS_FATFS_LFN=y` in the app's `prj.conf`; the manifest header says so.
