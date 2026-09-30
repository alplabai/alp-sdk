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
./gd32_ota_host --status --unbind
./gd32_ota_host --image gd32-bridge-slot-b.bin --version 0.2.22 --unbind
```

- The kernel `gpio-gd32-bridge` driver binds the bridge address, and i2c-dev
  refuses `I2C_SLAVE` on a bound address (EBUSY). `--unbind` detaches it via
  sysfs for the run and re-binds it at exit, so its traffic cannot
  interleave with OTA frames. Caveat: unbinding removes the bridge gpiochip
  while WiFi REG_ON and other bridge GPIOs may still be held; consumers do
  not re-acquire them after re-bind, so a reboot may be needed afterwards.
  A SIGINT/SIGTERM/SIGHUP during the run re-binds before exiting, even
  if it lands during the unbind itself.
- A run interrupted mid-session (SIGINT) leaves the bridge's OTA session
  open (state READY/BUSY/VERIFIED) until OTA_ABORT. The next run (not
  `--status`) sends OTA_ABORT at startup, prints it, re-reads the state and
  fails unless it is IDLE. ERROR is left for BEGIN to restart. The signal
  handler only re-binds; it sends no I2C.
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
