# iot-fleet-ota [UNTESTED]

Secure OTA firmware update with rollback. The production-readiness
proof for "how do we update 10 000 units in the field?".

Targets every E1M-X SoM family. native_sim build verified; HiL
verification gates on a staged Mender server (separate repo).

## What lands declaratively in v0.6

This example is the SDK's reference for the v0.6 `boot:` + `ota:`
declarative blocks. The relevant fragments of `board.yaml`:

```yaml
boot:
  method: mcuboot
  signing:
    algorithm: ecdsa_p256
    key_file:  keys/prod_ecdsa_p256.pub.pem
  swap_algorithm: scratch

ota:
  provider: mender
  artifact_name: iot-fleet-ota
  server:
    url:    "https://hosted.mender.io"
    tenant: "${MENDER_TENANT_TOKEN}"
  storage:
    device:        /dev/mmcblk0
    boot_part_mb:  32
    total_size_mb: 4096
  poll_interval_s: 1800
```

Both blocks are project-wide (one bootloader, one OTA fabric per
device). The orchestrator translates each into native build-system
config:

- `boot:` -> sysbuild MCUboot child image. `scripts/alp_orchestrate.py`
  emits `SB_CONFIG_BOOTLOADER_MCUBOOT=y`,
  `SB_CONFIG_MCUBOOT_SIGNATURE_TYPE_ECDSA_P256=y`,
  `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE="keys/prod_ecdsa_p256.pub.pem"`,
  and `SB_CONFIG_MCUBOOT_MODE_SWAP_USING_SCRATCH=y` into the
  sysbuild overlay.
- `ota:` -> Mender wiring. On Yocto slices the orchestrator writes
  `INHERIT += "mender-full"`, `MENDER_ARTIFACT_NAME`,
  `MENDER_SERVER_URL`, `MENDER_TENANT_TOKEN`,
  `MENDER_STORAGE_DEVICE_BASE`, `MENDER_BOOT_PART_SIZE_MB`,
  `MENDER_STORAGE_TOTAL_SIZE_MB`, and `MENDER_INVENTORY_INTERVAL`
  weak-assignments (`?=`) into `local.conf` -- hand-edited
  build-dir values still win. On Zephyr slices the
  mender-mcu-client west module reads the same fields at app
  init; `storage:` is ignored (MCUboot's slot geometry owns the
  on-device layout, not the OTA block).

The customer never hand-edits Kconfig or `local.conf` for the
OTA wiring; the board.yaml is the single source of truth.

## Trust model

Four pieces fit together:

- **OPTIGA Trust M** -- on-SoM secure element. Holds the
  ECDSA-P256 private key in tamper-resistant NVM (slot 0xE0F0).
  The private half is generated *inside* the chip at SoM-mfg
  time and never leaves. Without physical access to a
  provisioned OPTIGA, no attacker can produce a signature this
  device will accept. See [`docs/secure-boot.md`](../../../docs/secure-boot.md).
- **ECDSA-P256** -- the signing algorithm. The public half is
  read out of the OPTIGA once at provisioning, signed by the
  manufacturing CA, and compiled into the MCUboot bootloader
  (driven by `boot.signing.key_file:`). Same key gates both
  secure boot and OTA acceptance -- one trust root, fewer
  surfaces.
- **MCUboot slot-A/B** -- two-slot firmware layout with
  swap-using-scratch. The new image lands in the inactive slot;
  MCUboot re-verifies its signature at boot, swaps, hands off.
  Mid-swap power loss recovers atomically via the scratch
  sector.
- **Mender protocol** -- HTTPS-poll deployment fabric. Driven
  by the `ota:` block. Poll interval defaults to 30 min for
  battery-friendly nodes; this example sets it explicitly to
  1800 s for documentation clarity.

## Rollback semantics

The new image must call `boot_set_confirmed()` within its
health-check window (typically 30 s after boot, after a
successful Mender check-in). If it doesn't -- because it
crashes, hangs, or can't reach the server -- MCUboot's "test
pending" flag triggers an automatic rollback to the previous
slot on the next reboot. A bricking OTA undoes itself: the
watchdog-friendly safety net the fleet operator needs to sleep
at night.

## Linux variants

Customers forking this skeleton for V2N101 (Linux + M33) reuse
the entire `ota:` block as-is; the `storage:` subfields are
shaped for Mender's mmcblk0 A/B rootfs layout (32 MiB boot
partition, 4 GiB total -- two 2 GiB rootfs slots + persistent
data). Switching SoM is a `som.sku:` + `cores:` edit; the OTA
contract stays portable.

## Secrets

`${MENDER_TENANT_TOKEN}` is intentionally a placeholder. Real
tokens never live in the repo:

- Yocto: expanded by the build host's environment when the
  orchestrator-emitted `local.conf` is sourced.
- Zephyr: written into device-provisioning storage at first boot.

See [`docs/ota.md`](../../../docs/ota.md) "Secrets handling".

## References

- [`docs/secure-boot.md`](../../../docs/secure-boot.md) -- chain of
  trust, signing key lifecycle, key rotation playbook.
- [`docs/ota.md`](../../../docs/ota.md) -- Mender Zephyr client
  option (Option A) and the open delivery-half decision.
- [`docs/cc3501e-bridge.md`](../../../docs/cc3501e-bridge.md) --
  Wi-Fi transport on AEN.
- [`examples/connectivity/production-deployment`](../production-deployment/) --
  the broader lifecycle (factory ID + attestation + OTA in one app).
