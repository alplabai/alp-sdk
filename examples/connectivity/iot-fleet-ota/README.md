# iot-fleet-ota [UNTESTED]

Secure OTA firmware update, verify-only on the qualified E1M-AEN801
SKU (apply is DEFERRED, #1069 -- see [STATUS] in `board.yaml`). The
production-readiness proof for "how do we update 10 000 units in
the field?".

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
    key_file:  keys/mcuboot_dev_ecdsa_p256.pem
  # swap_algorithm: intentionally omitted -- E1M-AEN801's disjoint-slot0
  # `memory_map:` (#1069, #1413) has no slot1/scratch partition, so the
  # per-target default resolves to single-app boot.  Setting
  # `swap_algorithm: scratch` (or `move`/`overwrite`) explicitly here is
  # a build-time error on this SKU.

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

- `boot:` -> sysbuild MCUboot child image. `scripts/alp_orchestrate/`
  emits `SB_CONFIG_BOOTLOADER_MCUBOOT=y`,
  `SB_CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y`,
  `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE="keys/mcuboot_dev_ecdsa_p256.pem"`,
  and -- since this SKU's disjoint-slot0 `memory_map:` has no
  slot1/scratch partition -- `SB_CONFIG_MCUBOOT_MODE_SINGLE_APP=y`
  into the sysbuild overlay.
- `ota:` -> Mender wiring. On Yocto slices the planner writes
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
- **MCUboot boot chain** -- on this SKU (E1M-AEN801), MCUboot boots
  single-app (`SB_CONFIG_MCUBOOT_MODE_SINGLE_APP=y`, #1069/#1413):
  both M55 cores share the same physical App MRAM, so there is no
  inactive slot to swap the new image into. MCUboot re-verifies the
  slot0 image's signature on every boot and halts (rather than
  rolling back) if it fails. The classic two-slot swap-using-scratch
  model -- new image lands in the inactive slot, MCUboot swaps and
  hands off, mid-swap power loss recovers atomically via the scratch
  sector -- needs a two-slot AEN target, which none of the qualified
  boards are today.
- **Mender protocol** -- HTTPS-poll deployment fabric. Driven
  by the `ota:` block. Poll interval defaults to 30 min for
  battery-friendly nodes; this example sets it explicitly to
  1800 s for documentation clarity.

## Rollback semantics

On a genuine two-slot AEN target, the new image would call
`boot_set_confirmed()` within its health-check window (typically 30 s
after boot, after a successful Mender check-in); if it doesn't --
because it crashes, hangs, or can't reach the server -- MCUboot's
"test pending" flag would trigger an automatic rollback to the
previous slot on the next reboot.

**On this SKU (E1M-AEN801) that path does not exist.** Single-app
boot has no inactive slot to roll back to: a rejected or failed image
halts rather than rolling back (see the [STATUS] note in `board.yaml`
and `docs/secure-boot.md`). This example demonstrates the Mender +
signing wiring; the swap-with-revert rollback path needs a two-slot
AEN target, which none of the qualified boards are today.

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
