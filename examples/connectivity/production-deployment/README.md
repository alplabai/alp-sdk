# production-deployment

The v1.0 reference application for a field-deployable IoT
product.  Demonstrates the full manufactured -> deployed ->
updated -> attested lifecycle on a single app, so customers see
how the SDK's secure-boot, OTA, EEPROM-provisioning, and
remote-attestation pieces fit together.

This is also the SDK's **declarative-stack flagship**: every
v0.6 block (`boot:`, `ota:`, `security.psa:`, `storage:`,
`cores.<id>.memory:`, `cores.<id>.power:`,
`diagnostics.modules:`) appears in this `board.yaml` at the
production stance the SDK recommends for shipping product.

## The v0.6 block walkthrough

### `boot:` -- signed bootloader

```yaml
boot:
  method: mcuboot
  signing:
    algorithm: ecdsa_p256
    key_file:  keys/prod_ecdsa_p256.pub.pem
  slots:
    primary:   { size_kib: 1024 }
    secondary: { size_kib: 1024 }
  swap_algorithm:   scratch
  scratch_size_kib: 64
  anti_rollback:    true
```

Drives sysbuild's MCUboot child image. ECDSA-P256 ties to the
OPTIGA Trust M production key (see `iot-fleet-ota`).
`anti_rollback: true` enables monotonic image counters -- this
requires OTP fuse provisioning at factory test (one-way; cannot
be backed out in the field).

### `ota:` -- Mender HTTPS poll + A/B rollback

```yaml
ota:
  provider:        mender
  artifact_name:   production-deployment
  server:
    url:    "https://hosted.mender.io"
    tenant: "${MENDER_TENANT_TOKEN}"
  rollback:
    enabled:     true
    min_version: 1
  poll_interval_s: 1800
  storage:
    device:        /dev/mmcblk0
    boot_part_mb:  32
    total_size_mb: 4096
```

`rollback.min_version: 1` is the anti-downgrade floor -- once v1.0
ships, the device refuses any OTA claiming version < 1, even if
it's signed correctly. `${MENDER_TENANT_TOKEN}` never lives in
the repo; it's injected at provisioning.

### `security.psa:` -- TF-M + OPTIGA attestation root

```yaml
security:
  psa:
    persistent_slots: 32
    its_storage:      mram_main
    ps_storage:       ospi0
    tfm:              true
    attestation_root: optiga_trust_m
```

`tfm: true` lands TF-M's secure-partition image as a sysbuild
child build. Internal Trusted Storage (PSA persistent keys) backs
to the secure half of MRAM; Protected Storage (encrypted-at-rest
app credentials) backs to the on-module OSPI. The attestation
root is the OPTIGA Trust M -- single trust root with boot + OTA,
fewer surfaces for an attacker to chip away at.

### `storage:` -- explicit partition table

```yaml
storage:
  - { name: mcuboot_primary,   fs: raw,      size_kib: 1024, flash_device: mram_main }
  - { name: mcuboot_secondary, fs: raw,      size_kib: 1024, flash_device: mram_main }
  - { name: mcuboot_scratch,   fs: raw,      size_kib:   64, flash_device: mram_main }
  - { name: settings,          fs: littlefs, size_kib:   64, flash_device: mram_main, mount: /lfs/settings }
  - { name: app_data,          fs: littlefs, size_kib:  256, flash_device: mram_main, mount: /lfs/app }
```

The MCUboot slots are explicit (matching `boot.slots:` sizes);
Zephyr's settings subsystem gets its own littlefs partition;
app-managed runtime data gets its own. Adds to ~2.4 MiB of the
AEN E7's 5.5 MiB MRAM -- the rest stays free for code + MCUboot
itself + TF-M's secure partition. The orchestrator emits a
partial DTS overlay (`partitions { ... }` node) + matching
Kconfig (`CONFIG_FILE_SYSTEM_LITTLEFS=y`) per entry.

### `cores.m55_hp.memory:` -- per-core memory tuning

```yaml
memory:
  stack_kib:     8     # CONFIG_MAIN_STACK_SIZE
  heap_kib:      64    # CONFIG_HEAP_MEM_POOL_SIZE
  isr_stack_kib: 4     # CONFIG_ISR_STACK_SIZE
```

Production stance: enough heap for mbedTLS handshake buffers +
mender-mcu-client state; enough stack for the TF-M NS-callable
trampoline.

### `cores.m55_hp.power:` -- standby with wake-on-network

```yaml
power:
  sleep_mode: standby
  wakeup_sources: [uart, gpio, rtc]
```

Application cores run `standby` rather than `deep` so the
mender-mcu-client poll thread resumes without losing TLS state.
The low-power case (deep sleep + sensor-driven wake) is in
`examples/power-timing/power-managed-sensor`.

### `diagnostics.modules:` -- per-module log levels

```yaml
diagnostics:
  log_level: warn
  modules:
    alp_security: info
    alp_iot:      info
```

Field console captures TLS handshake + Mender state transitions;
the rest stays quiet to save flash and console bandwidth.

## Build

### native_sim (framing test, no real ops)

```bash
west build -b native_sim/native/64 examples/connectivity/production-deployment
west build -t run
```

### Real silicon (AEN-Zephyr, requires a staged Mender server)

```bash
west alp-build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/connectivity/production-deployment
west flash
```

On HiL the full lifecycle runs: boot from a factory-signed
image, read the EEPROM manifest, inspect MCUboot slots, connect
to the board-staged Mender server, poll for an update.  When
a deployment lands the SDK downloads + verifies + applies it,
requests reboot, then confirms post-reboot.  Attestation
heartbeats publish every 60 s thereafter.

## Production variants

Customer-side variants typically:

- Fork this skeleton for V2N or i.MX 93 boards (`som.sku:` +
  `cores:` edits; the declarative blocks above stay portable).
- Replace the Mender connection with a different OTA fabric
  (`ota.provider:` -- `mcumgr` support tracked in ADR 0009).
- Add domain-specific business logic between the OTA poll +
  the attestation heartbeat.

## Reference

- [`<alp/hw_info.h>`](../../../include/alp/hw_info.h) -- factory
  EEPROM manifest read-back.
- [`<alp/storage.h>`](../../../include/alp/storage.h) -- MCUboot
  slot inspection + OTA chunk write.
- [`<alp/iot.h>`](../../../include/alp/iot.h) -- Wi-Fi + MQTT +
  TLS for the Mender connection.
- [`<alp/security.h>`](../../../include/alp/security.h) -- AEAD
  + TRNG primitives for attestation.
- [`docs/secure-boot.md`](../../../docs/secure-boot.md) -- the
  ECDSA-P256 signing-service contract.
- [`docs/threat-model.md`](../../../docs/threat-model.md) §asset 8
  -- the tamper-evidence requirement.
- [`docs/tutorials/12-mender-ota.md`](../../../docs/tutorials/12-mender-ota.md)
  -- step-by-step walkthrough of the Mender path.
- [`docs/v1.0-readiness.md`](../../../docs/v1.0-readiness.md) §4
  -- this example is the production-deployment flagship.
