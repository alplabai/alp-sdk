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
    key_file:  keys/mcuboot_dev_ecdsa_p256.pem
```

Drives sysbuild's MCUboot child image. ECDSA-P256 ties to the
OPTIGA Trust M production key (see `iot-fleet-ota`). `swap_algorithm:`
is intentionally omitted: E1M-AEN801's disjoint-slot0 `memory_map:`
(#1069, #1413) has no slot1/scratch partition, so the SDK's per-target
default resolves to single-app boot
(`SB_CONFIG_MCUBOOT_MODE_SINGLE_APP=y`); setting `swap_algorithm:
scratch` (or `move`/`overwrite`) explicitly here is a build-time error
on this SKU. Slot/scratch partition *sizes* aren't a `boot:` field
either way -- MCUboot takes its geometry from the board DT
`partitions {}` node, not from `storage:` (see the `storage:` section
below). Downgrade prevention is the `ota.rollback.min_version`
software floor below; a hardware anti-rollback counter tier
(OPTIGA/OTP fuse) isn't built yet, so this skeleton doesn't claim one.

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

`mcuboot_primary` / `mcuboot_secondary` / `mcuboot_scratch` are named
to illustrate a two-slot MCUboot layout, but they are `storage:`
partitions, NOT the board DT's `slot0_partition` / `slot1_partition`
/ `scratch_partition` labels MCUboot's flash-map actually reads --
MCUboot itself boots single-app on this SKU (see the `boot:` section
above). Sizes sum to ~2.4 MiB (1024 + 1024 + 64 + 64 + 256 KiB), but
on E1M-AEN801 there is no remainder to place them in: the SoM's own
`memory_map:` regions already occupy the full 5632 KiB App MRAM
window (`metadata/e1m_modules/E1M-AEN801.yaml`), so EVERY entry above
emits `status: blocked` in the generated `dts-partitions.dtsi`
(verify with `--emit dts-partitions`) for that reason alone.
`mram_main` would ALSO not be a working `flash_device:` target even
with free room: no AEN preset declares a `dt_label:` override for it,
so it resolves to a Devicetree label of `mram_main`, but the
generated board tree never defines that node -- only `mram_storage`
(alp-sdk#1484; see `docs/board-config-features.md`'s "Storage
partitions (`storage:`)" section). This section demonstrates
the `storage:` declarative shape only, not a working layout, on this
SKU.

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
tan build --project examples/connectivity/production-deployment
west flash
```

On HiL the qualified path runs: boot from a factory-signed
image, read the EEPROM manifest, inspect MCUboot slots, connect
to the board-staged Mender server, poll for an update.  When a
deployment lands the SDK downloads and verifies it -- but on
this SKU (E1M-AEN801) OTA *apply* is DEFERRED (#1069, see
[STATUS] in `board.yaml`): there is no secondary/scratch slot to
write, and self-overwriting the running slot0 is not a supported
flow, so the SDK stops after verification and does not write or
reboot.  Attestation heartbeats publish every 60 s regardless.

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
  slot inspection; OTA chunk write is DEFERRED on this SKU
  (#1069, see [STATUS] in `board.yaml`).
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
