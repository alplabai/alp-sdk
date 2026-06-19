<!-- SPDX-License-Identifier: Apache-2.0 -->
# CC3501E production image & provisioning

How to build, sign, and provision a **shippable** CC3501E coprocessor image for the
E1M-AEN SoM. The CC3501E runs ALP-authored firmware on its own Cortex-M33 (embedded
in this repo per ADR 0015); the host (Alif E8) talks to it over the inter-chip SPI
bridge (`docs/cc3501e-bridge.md`).

## Image contents

The production firmware is the **full** build — Wi-Fi + BLE + bridge + OTA:

```
firmware/cc3501e/ti/build_ti.ps1 -Ble      # -Ble implies -WifiHostDriver
```

Output: `firmware/cc3501e/build/ti/cc3501e-bridge.{out,hex,bin}` (~1.0 MB text,
~309 KB bss; fits the 512 KB DRAM). This is the same firmware as the bench bridge
image plus the Wi-Fi host driver + NimBLE host (sized for a bridge peripheral).

## Signing — HSM root of trust

**Production images are signed by the HSM**, which holds the production root key. The
HSM is *not* on a dev/bench machine, so signing is the one step a developer cannot do
locally. Bench/staging uses the **Alp VALIDATION** key (`deploy_validate.ps1`); those
units are validation/staging only and are **NOT production-shippable** (rooted to the
validation key, not the HSM).

Reproducible production packaging (HSM operator):

```
firmware/cc3501e/ti/package_cc3501e_prod.ps1 \
    -PublicKey     <hsm_production_pub.pem> \
    -SigningModule <hsm_sign.py> \
    -ToolboxExe    <simplelink-wifi-toolbox.exe> \
    -ConfBin       <cc35xx-conf.bin> \
    -Version       0.1.0.0 \
    -RollbackProtection -Program -XdsSerial <XDS110_SN>
```

It builds the firmware, wraps it as a signed GPE vendor image with the HSM key, and
(with `-Program`) factory-programs a fresh unit.

## Unit activation — must be cold-bootable

`factory_programming` activates a **FRESH** (never-activated) part: it burns the HSM
public key as the RoT (and, with `-RollbackProtection`, the anti-rollback fuses) and
writes the boot sector / Protected-Storage / vendor image atomically. An
already-activated part rejects it (`-1141`).

**Activate with `vendor_sbl_container_enable=0`** (or ship a TI vendor SBL). The bench
unit was activated with that fuse set but with **no** vendor SBL, which breaks the cold
Chain-of-Trust → the image never launches on a cold POR (and the OTA swap-boot, which
runs the same cold chain via BL2, cannot complete). See
`memory/project-cc3501e-firmware-bringup` and `project-cc3501e-ota-bridge-rootcause`.

## OTA

OTA-over-the-bridge (host streams a signed vendor image → `psa_fwu` → MCUboot swap) is
implemented and silicon-validated through **install/STAGED** (`chips/cc3501e/cc3501e.c`,
`firmware/cc3501e/hal/ti/cc3501e_hw_ti.c`). Each OTA payload is itself a signed vendor
image (same FIB+sign recipe) whose version must exceed the running primary. The final
cold **swap-boot** depends on the unit being cold-bootable (above), so it completes on
correctly-activated production units; it does not on the mis-activated bench unit.

## Status / open items (2026-06-19)

- ✅ Full firmware (Wi-Fi+BLE+bridge+OTA) builds + links.
- ✅ OTA receive → stage → install (STAGED) silicon-validated; bridge transport reliable.
- ⏳ **Wi-Fi / BLE radio bring-up**: to be verified once antenna-tuning components arrive
  (the firmware links the stacks; on-air not yet validated).
- ⏳ **Production signing**: HSM step (key not on the bench).
- ⏳ **OTA cold swap-boot**: requires a correctly-activated (`vendor_sbl_container_enable=0`)
  production unit; gated on the bench unit only.
