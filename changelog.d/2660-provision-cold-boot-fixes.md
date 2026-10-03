### Fixed — provisioning: cold_boot_test survives a kernel printk after the prompt, records the firmware versions and keeps the passed-cycle count (#2660)

- The console shell-prompt wait now resyncs: a window with no match sends a newline (bounded
  by `RESYNC_TRIES`) so the shell prints a fresh prompt, instead of failing a cold cycle on
  `root@e1m-v2m103:~# [   13.500308] Bluetooth: MGMT ver 1.22`. The prompt patterns stay anchored.
- `cold_boot_test` records `bl2_version`, `bl31_version` and `uboot_version` from each cycle's
  boot transcript (the last passing cycle's values) and fails on a change between cycles.
- `cold_boot_test` failing mid-way keeps the evidence of the cycles that passed:
  `cold_boots_passed` reads e.g. `2/3`, not a stale `0/3`.
- `gd32_flash` has a test pinning that the path that flashes records `gd32_fw_version` too.
- `docs/provisioning-v2n.md` documents the two-leg DSW1/microSD operator flow (xSPI mode throughout), unit identification before provisioning, SCPI pacing and the accepted `eth_phy_id` list (RTL8211F and RTL8211F-VD).
