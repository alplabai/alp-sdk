# `board.yaml` build-system integration knobs

Structured `board.yaml` blocks that replace hand-edited
`prj.conf` / `local.conf` / sysbuild.conf options: the hardware-info
EEPROM location, per-slice memory + power tuning, per-module log
levels, the MCUboot bootloader block, OTA (Mender / hawkBit /
MCUmgr), fixed storage partitions, and PSA Crypto + TF-M.  All
optional; the SoM family / SDK baseline supplies defaults when
omitted.

See [`docs/board-config.md`](board-config.md) for the landing page.

## Build-system integration knobs

For declarative coverage of build options that customers used to
hand-edit in `prj.conf` / `local.conf` / sysbuild.conf,
`board.yaml` carries a small set of structured blocks.  All
optional; sensible defaults from the SoM family / SDK baseline
apply when omitted.

### Hardware-info EEPROM (`features.hw_info.eeprom:`)

```yaml
features:
  hw_info:
    eeprom:
      bus: e1m_i2c0        # -> CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID=0
      addr_7bit: 0x50      # CONFIG_ALP_SDK_HW_INFO_EEPROM_ADDR_7BIT
      offset: 0            # CONFIG_ALP_SDK_HW_INFO_EEPROM_OFFSET
```

Project-wide.  This declares where the 128-byte
`alp_hw_info_eeprom_t` manifest lives when an app needs to pin the
EEPROM bus/address/offset explicitly.  The orchestrator emits the
values into Zephyr `alp.conf` and projects them into
`system-manifest.yaml` under `hw_info.eeprom`.

`features:` is not a free-form pass-through.  Only typed feature
blocks with emitter support are accepted; unsupported keys are schema
errors so configuration cannot be silently ignored.

### Per-slice memory (`cores.<id>.memory:`)

```yaml
cores:
  m55_hp:
    app: ./src
    memory:
      stack_kib:     8       # CONFIG_MAIN_STACK_SIZE  = 8192
      heap_kib:      4       # CONFIG_HEAP_MEM_POOL_SIZE = 4096
      isr_stack_kib: 2       # CONFIG_ISR_STACK_SIZE  = 2048
```

Zephyr slice only; ignored on Yocto / baremetal.  Replaces
hand-edited `CONFIG_MAIN_STACK_SIZE` etc. in `prj.conf`.

### Per-slice power (`cores.<id>.power:`)

```yaml
cores:
  m55_he:
    app: ./src
    power:
      sleep_mode:     standby      # disabled | idle | standby | deep
      wakeup_sources: [uart, gpio, rtc]
```

`sleep_mode != disabled` emits `CONFIG_PM=y` + `CONFIG_PM_DEVICE=y`
and lands the per-state hierarchy.  `wakeup_sources:` entries that
name a subsystem (`uart`, `gpio`, ...) emit
`CONFIG_PM_DEVICE_WAKE_<SUBSYS>=y`; `E1M_*` pad names emit a hint
comment (per-silicon wake-pin Kconfig lands in v0.7).

### Per-module log levels (`diagnostics.modules:`)

```yaml
diagnostics:
  log_level: info               # default for everything else
  modules:
    alp_iot:       debug
    alp_inference: warn
    alp_security:  "off"        # quote 'off' -- bare off parses as YAML false
```

Emits `CONFIG_<MODULE>_LOG_LEVEL=<n>` per entry into the slice's
`alp.conf`.  Saves customers from finding the right
`CONFIG_LOG_*` symbol per module.

### Bootloader (`boot:` -- MCUboot)

```yaml
boot:
  method: mcuboot                # mcuboot | none
  signing:
    algorithm: ecdsa_p256        # ecdsa_p256 | rsa2048 | rsa3072 | ed25519
    key_file: keys/prod_ecdsa_p256.pub.pem
  swap_algorithm: scratch        # scratch | move | overwrite
```

Project-wide (one bootloader per device).  The loader emits a
sysbuild.conf overlay with the corresponding `SB_CONFIG_*` lines
(MCUboot signature type, swap algorithm).  See `docs/secure-boot.md`
for the underlying secure-boot contract.

There is no `slots:` / `scratch_size_kib:` / `anti_rollback:` field.
Slot and scratch partition *sizes* are an SDK build-policy choice, not
a per-project field -- MCUboot takes its geometry from the board DT
`partitions {}` node (declare the actual layout via `storage:` below
if you want it explicit in your `board.yaml`); sysbuild has no
`SB_CONFIG_*` symbol for a partition size at all.  `anti_rollback` was
removed rather than fixed: only software downgrade prevention
(`ota.rollback.min_version`) is wired today, and the field's own
description promised the OTP-fused hardware-counter tier, which
isn't built -- a silent SW substitute would have shipped weaker
security than the schema claimed.  For `method: mcuboot`, `rsa3072`
is rejected at emit time (sysbuild's RSA choice has no key-length
knob); use `rsa2048` or `ecdsa_p256`/`ed25519`.

### OTA (`ota:` -- Mender / MCUmgr)

```yaml
ota:
  provider: mender               # mender | mcumgr | none
  artifact_name: my-product-1.2.3
  signing_key: keys/mender_artifact.pem
  server:
    url:    https://hosted.mender.io
    tenant: my-tenant
  rollback: { enabled: true, retries: 3, min_version: 1 }
  poll_interval_s: 1800
  storage:
    device: /dev/mmcblk0p        # Yocto-only
    boot_part_mb: 64
    rootfs_ab: true
    total_size_mb: 4096
```

Project-wide, provider-driven dispatch (ADR 0009 resolved):

| Provider   | Yocto emit                              | Zephyr emit                                                                       |
|------------|-----------------------------------------|-----------------------------------------------------------------------------------|
| `mender`   | `MENDER_*` weak-assigns in `local.conf` | `CONFIG_MENDER_MCU_CLIENT=y` + URL/tenant/poll Kconfig (out-of-tree, west.yml)    |
| `hawkbit`  | n/a (Zephyr only)                       | `CONFIG_HAWKBIT=y` + `HAWKBIT_SERVER` + `HAWKBIT_POLL_INTERVAL` (Zephyr upstream) |
| `mcumgr`   | n/a (Zephyr only)                       | `CONFIG_MCUMGR=y` + GRP_IMG/GRP_OS (transport is the app's call)                  |
| `none`     | OTA disabled                            | OTA disabled                                                                      |

The validator (P2.3 rule 1) requires:
- `provider: mender` → at least one `cores.<id>.os: yocto` OR `cores.<id>.os: zephyr`
- `provider: hawkbit` → at least one `cores.<id>.os: zephyr`
- `provider: mcumgr` → at least one `cores.<id>.os: zephyr`

When `provider: mender` and at least one Zephyr slice is present,
the west-libraries emitter (`--emit west-libraries`) auto-adds a
`mender-mcu-client` entry to the `name-allowlist:` so the customer's
`west update` pulls the module without hand-edits.  Hawkbit and
MCUmgr are upstream Zephyr modules so they don't need a west.yml
entry.

See `docs/ota.md` + `docs/ota-device-contract.md` + ADR 0009.

### Storage partitions (`storage:`)

```yaml
storage:
  - { name: settings,        fs: littlefs, size_kib: 64,  mount: /lfs/settings,
      flash_device: mram_main }
  - { name: app_data,        fs: littlefs, size_kib: 128, mount: /lfs/app,
      flash_device: mram_main }
  - { name: mcuboot_scratch, fs: raw,      size_kib: 32,
      flash_device: mram_main }
  - { name: pinned_low,      fs: raw,      size_kib: 32,
      flash_device: mram_main, offset_kib: 0 }    # explicit offset override
```

Project-wide.  Each entry declares a fixed partition on the
referenced flash device.  Partitions on the same device are
**name-sorted** and allocated **bottom-up, page-aligned to 4 KiB**
so layouts stay byte-stable across rebuilds (the address determinism
property OTA images depend on).  Supply `offset_kib:` to pin an
entry to an explicit offset within the device -- useful for
coexistence with bootloader-managed slots or when migrating a
legacy layout.

`flash_device:` resolves against the SoM preset in this order:

1. `memory_map:` region names (e.g. `mram_main`, `ocram_low`) --
   either declared on the SoM or auto-derived from the SoC variant's
   `mram_mb` / `sram_banks_kb` (the same resolution `resolve_memory_map()`
   does for IPC carve-outs).
2. `on_module.ospi_memories:` keys (e.g. `ospi0`) -- external OSPI
   flash declared on the SoM.

The loader rejects typoed `flash_device:` references at parse time
with the list of known devices for the project's SoM.  When the
referenced device's size is `TBD` (HW-config still owed), the
resolver projects the entry as `status: blocked` in the generated
manifest with a reason that points at the SoM file owing the value.

The orchestrator emits two artefacts per project:

* `dts-partitions.dtsi` -- a DTS overlay that decorates each
  referenced flash device with a `partitions { compatible =
  "fixed-partitions"; ... };` child node carrying every resolved
  partition's `label`, `reg`, and DT phandle (`<name>_partition`).
  Materialised under `build/generated/` alongside
  `dts-reservations.dtsi` (IPC).
* Per-fs Kconfig in each Zephyr slice's `alp.conf`:
  `CONFIG_FILE_SYSTEM=y`, plus `CONFIG_FILE_SYSTEM_LITTLEFS=y` /
  `CONFIG_FAT_FILESYSTEM_ELM=y` / `CONFIG_FILE_SYSTEM_EXT2=y`
  per the `fs:` enum, plus `CONFIG_FS_LITTLEFS_PARTITION_<NAME>=y`
  per littlefs entry.

An optional static C mount table (`--emit storage-mounts-c`)
generates a `fs_mount_t` per entry with a `mount:` declared, plus an
aggregate `alp_storage_mounts[]` array for boot-time iteration.

Inspect the resolved layout with:

```bash
PYTHONPATH=scripts python3 -m alp_orchestrate --input board.yaml --emit dts-partitions
PYTHONPATH=scripts python3 -m alp_orchestrate --input board.yaml --emit system-manifest \
    | yq '.storage[]'
```

The `system-manifest.yaml` carries every resolved partition's
`offset_kib`, `size_kib`, `dt_label`, `mount`, and (when blocked)
`reason:` so reviewers see the full flash map alongside IPC carve-outs.

### PSA Crypto + TF-M (`security.psa:`)

```yaml
security:
  psa:
    persistent_slots:  16
    its_storage:       mram_main          # SoM memory_map region OR storage[] name
    ps_storage:        ospi0              # optional (PS)
    tfm:               true               # enable TF-M secure partition
    attestation_root:  optiga_trust_m     # optiga_trust_m | tfm_internal | none
```

Project-wide.  When `tfm: true` the orchestrator emits a sysbuild
child-image overlay at `build/sysbuild/tfm/tfm.conf` containing
`SB_CONFIG_TFM=y`, `SB_CONFIG_TFM_BUILD_TYPE=<Release|Debug|MinSizeRel>`
(inherited from `boot.build_type` when set), the
`CONFIG_PSA_CRYPTO_PERSISTENT_SLOT_COUNT` value, and string-form
`CONFIG_PSA_CRYPTO_{ITS,PS}_BACKING_STORE` references resolving to a
`storage[].name` or a SoM `memory_map:` region.  When `tfm: false`
PSA Crypto runs entirely non-secure (mbedTLS-only) and the overlay is
not written.

`attestation_root: optiga_trust_m` only validates when the SoM preset
physically ships OPTIGA Trust M (AEN family + V2N family today); the
emitter additionally surfaces `CONFIG_ALP_SDK_PSA_ATTESTATION_OPTIGA=y`
and a comment pointing at the `src/security/optiga_trust_m_bridge.c`
PSA <-> OPTIGA bridge driver.

The TF-M secure partition runs on the same M55-HP core as the
non-secure app via the Armv8-M security extension (TrustZone-M split,
not a separate M55-HE core).  See `docs/adr/0013-tfm-boundary-m55-hp-trustzone.md`
+ `docs/security-audit-plan.md` + ADR 0010.

