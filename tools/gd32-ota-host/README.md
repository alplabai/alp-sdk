# gd32_ota_host

Linux (A55) host-side OTA driver for the GD32 bridge, built on the portable
`gd32g553_ota_*` API over the yocto i2c-dev backend. Prints BEGIN
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
  interleave with OTA frames.
- The image must be the slot the bridge is NOT running (BEGIN reports the
  target slot). `--version` is recorded in the A/B metadata
  `fw_version[slot]` at COMMIT.
- `--no-commit` stops after VERIFY. Default bus `8`, address `0x70`.
