### Added

- `provision_som.py`: `dxm1_npu_flash` is now a real step. It programs the V2M
  DX-M1 NPU firmware through the ROM's UART path (dxuart2 DTB swap and warm
  reboot, `dxflash.py` with the PA6 reset pulse, release DTB restored and
  md5-checked, clean poweroff and cold cycle), verifies PCIe device `0x0000`
  plus the `dxrt-cli -s` version, and records `dxm1_fw_version`,
  `dxm1_fw_md5` and `dxm1_fw_uart_boot_md5`. A wrong BOOT_CFG strap is
  reported from the ROM output. `census` reads `dxm1_pcie_device` and
  `dxm1_fw_version` on any unit. The release bundle schema gains the optional
  `dxm1_*` component roles and a component `version`; the license-gated
  files stay in the private bundle. See `docs/provisioning-v2n.md`.

### Removed

- `provision_som.py --enable-dxm1-flash` and the vendor `uart_boot` path it
  gated; the step runs whenever the bundle carries the DX-M1 set.

### Fixed

- `provision_som.py`: the `write_xspi`, `write_emmc_boot`, `write_rootfs`,
  `eeprom_manifest` and `secure_page` probes attach the pinned bench.yaml host
  like `census` does, so `--only write_xspi` reports an already-flashed unit
  as satisfied instead of planning a flash (which also made `record` withhold
  the bundle facts).
- `provision_som.py`: `boot_sd_linux` no longer reports done with no Linux
  target attached. After a console login without an IPv4 host it recognises
  the end0 PHY latch (#2582: the stmmac `Failed to reset the dma` line, or end0
  down) and runs ONE extra cold cycle under the usual `MIN_OFF_S` rules, else
  waits up to 120 s for a slow DHCP lease; it then fails with a message naming
  #2582 and the dmesg line, or "no IPv4 on end0 ... check cable/DHCP". Only a
  pending `--gd32-fw` flash (blank GD32: no RX clock) keeps the console-only
  path. `write_rootfs` and the other steps report why no target is attached
  instead of "boot_sd_linux has not run".
