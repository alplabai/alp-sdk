### Changed — V2N/V2M U-Boot: the environment now persists (redundant pair in eMMC boot partition 2), and an empty microSD slot no longer prints a failure

- **`saveenv` survives a reboot.** `CONFIG_BOOTCOMMAND` no longer opens with `env default -a`. The environment is a redundant pair in eMMC boot partition 2 (`/dev/mmcblk0boot1`, offsets `0x220000` and `0x230000`, `0x10000` each; boot partition 1 keeps the bootloader). U-Boot patch `0014` writes the defaults once on first boot and re-applies `bootcmd` from the binary every boot, so a saved copy from an older U-Boot cannot go stale. The vendor default location (end of the eMMC user area) is no longer used.
- **Linux can read and write it.** Images for the V2N family carry `libubootenv` (`fw_printenv`, `fw_setenv`) and an `/etc/fw_env.config` matching the offsets, for the OTA client to switch slots. `tests/scripts/test_uboot_env_layout.py` fails if the U-Boot config, `fw_env.config` and the provisioning ceiling disagree.
- **Provisioning cannot erase it.** The Linux eMMC boot write refuses an image that would reach offset `0x220000`.
- **Empty microSD slot is silent.** U-Boot patch `0013` reads the card-detect pin (PA1, active-low) and skips probing `mmc1` when no card is in, removing `Card did not respond to voltage select! : -110`.

Not run on silicon; the bench steps are in `docs/bring-up-v2n.md` section 7a. Design in `docs/soms/v2n.md#uboot-environment`.
