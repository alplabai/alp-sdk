# gd32_ota_host

Linux (A55) host-side OTA driver for the GD32 bridge, built on the
gd32g553 chip-driver `gd32g553_ota_*` API over the yocto i2c-dev backend. Prints BEGIN
`chunk_max`, VERIFY crc, COMMIT status and the post-reboot build id.

Build (aarch64, against a yocto-backend `libalp_sdk.a`):

```sh
$CC -I include tools/gd32-ota-host/gd32_ota_host.c chips/gd32g553/gd32g553.c \
    build-sdk/libalp_sdk.a -o gd32_ota_host
```

Run on the target (root):

```sh
./gd32_ota_host --status
./gd32_ota_host --image gd32-bridge-slot-b.bin --version 0.2.22
```

- The kernel `gpio-gd32-bridge` driver stays bound. Frames use `ioctl(I2C_RDWR)`,
  which i2c-dev allows on a bound address (only `I2C_SLAVE` needs it free, and
  this flow never uses it). Never unbind it by hand: its consumers
  (`wlan-pwrseq`, `hci_bcm`, the panel) hold its lines, and an unbind leaves a
  dangling `mmc_pwrseq` that panics the next reboot (#2734).
- **Reboot after COMMIT.** The tool prints `reboot required`: the GD32 resets
  twice, the Wi-Fi chip loses power, and brcmfmac cannot recover without a
  board reboot.
- A run interrupted mid-session (SIGINT) leaves the bridge's OTA session
  open (state READY/BUSY/VERIFIED) until OTA_ABORT. The next run (not
  `--status`) sends OTA_ABORT at startup, prints it, re-reads the state and
  fails unless it is IDLE. ERROR is left for BEGIN to restart.
- A chunk that times out twice is re-sent at half the length, 8-aligned
  (56, 32, 16, 8), then full-size chunks resume. If BEGIN's reply was lost,
  chunks default to 56 bytes.
- The image must be the slot the bridge is NOT running (BEGIN reports the
  target slot). `--version` is recorded in the A/B metadata
  `fw_version[slot]` at COMMIT (the `after:` line prints the protocol
  version, not that triple).
- `--no-commit` stops after VERIFY. Default bus `8`, address `0x70`.
- `RESULT: PASS` after COMMIT requires the post-reboot state to read back,
  state not ERROR, active slot == the BEGIN target slot (and different from
  the pre-run slot) and a changed build id. A rejected or reverted COMMIT
  reports FAIL.
- Not built by CI or CMake; rebuild it when the `gd32g553_ota_*` API changes.
