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

## Bench warm-program (validation flash)

The operative flash for **iterating** the CC3501E firmware on the bench. The bench
unit's cold boot chain is fuse-broken (activated with `vendor_sbl_container_enable=1`
but no vendor SBL — see *Unit activation* below), so a cold POR never launches a
freshly-programmed image. The **warm** path works: the Alif host app warm-resets the
CC3501E on its own boot, and the warm reset skips the vendor-SBL chain, so the image
runs. No PSU cold-cycle.

Recipe (rooted to the **Alp VALIDATION** vendor key — staging only, see the warning
below). Each step is one `simplelink-wifi-toolbox` (TI Wi-Fi toolbox) invocation:

1. **Build** the full image: `firmware/cc3501e/ti/build_ti.ps1 -Ble`
   → `firmware/cc3501e/build/ti/cc3501e-bridge.out`.
2. **FIB build** a vendor image at a **monotonically increasing** version — the
   anti-rollback fuses reject any version `<=` the one already programmed:
   `flash-images-builder build vendor_image --version <X.Y.Z.W>
   --public_key <validation_pub.pem> --vendor_out_file <…/cc3501e-bridge.out>
   --conf_bin_file <cc35xx-conf.bin>`.
3. **Sign** with the validation vendor key: `flash-images-builder sign vendor_image
   --activation_type vendor_key --signing_module <sign.py> --public_key <validation_pub.pem>`,
   then copy `vendor_image.sign.bin` → `primary_vendor_image.sign.bin`.
4. **Program** over XDS110, retrying on `-1141` (the SES maintenance window is
   transient): `programmer -i XDS110 -param1 <XDS110_SN> programming
   --tool_settings <tool_settings.json>`.
5. **Verify** on the host: power-cycle the carrier, then exercise the bridge from the
   Alif console — `alp companion ver`, `alp companion wifi scan`,
   `alp companion ble enable`, `alp companion ble scan`.

The bench helper `fib_program_warm.ps1 -Version <X.Y.Z.W>` chains steps 2–4; bump the
version on **every** flash. Keys live outside the repo (never committed); reference
them by role, not path.

> Validation-key images are **staging only** — NOT production-shippable (rooted to the
> VALIDATION key, not the HSM). Production uses `package_cc3501e_prod.ps1` + the HSM.

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
implemented and **silicon-validated end-to-end** (`chips/cc3501e/cc3501e.c`,
`firmware/cc3501e/hal/ti/cc3501e_hw_ti.c`). Each OTA payload is itself a signed vendor
image (same FIB+sign recipe) whose version must **exceed** the running primary — monotonic
anti-rollback: a downgrade is refused at `psa_fwu` install (`OTA_STATUS state=3` ERROR), a
forward image is accepted (`state=2` STAGED). The swap is completed by the CC35's OWN
`psa_fwu_request_reboot()` after FINISH (NOT a host cold POR). **Proven 2026-07-10 on the
E1M-AEN801 EVK:** a forward candidate streamed → `state=2` STAGED → self-reboot swap (bridge
dropped ~2 s then returned) → the swapped image self-accepted and **persisted across a true
cold POR** (no rollback). The `OTA_STATUS reserved[0]` byte surfaces the swap-reboot rc
(0 = success, non-zero = refused).

## Status / open items (2026-06-24)

- ✅ Full firmware (Wi-Fi+BLE+bridge+OTA) builds + links (`-Ble`, 0 errors).
- ✅ **Wi-Fi SCAN ON-AIR validated** (2026-06-22, warm-programmed v0.0.161.0): `wifi scan`
  returns real APs with RSSI + decoded **open / WPA2 / WPA3** security (the scan packs
  the raw 16-bit TI `SecurityInfo`; the sec-type lives in its high byte).
- ✅ **Wi-Fi CONNECT — async, validated on silicon (2026-06-24).**
  `wifi connect` is plumbed end-to-end (host → bridge → `Wlan_Connect`), runs
  **async** (worker-routed off the SPI ISR; the host shell stays live during the
  ~15 s WPA3 association), and the L2 association completes with the bridge intact.
  The earlier "the ~15 s association **desyncs the CS-less r1 SPI bridge permanently**"
  limitation is **resolved**: the current rev runs a **hardware peripheral-driven SS0
  chip-select** (Alif `P14_7` = `SPI1_SS0_C`; dwc-ssi drives SS0 per transfer) plus
  per-phase READY gating, so a busy radio can no longer lose link framing — `ver`
  after a connect still returns, no power-cycle needed (`docs/cc3501e-bridge.md`
  "Bench-validated").  Connect was also found to have been dispatched **synchronously
  in the SPI ISR** (every other blocking radio op is worker-routed); that is fixed
  (worker-routed).
- ✅ **BLE ON-AIR validated** (2026-06-22): `ble enable` brings the NimBLE host up;
  `ble scan` (`ble_gap_disc`) discovers real advertisers (address + RSSI + parsed name,
  e.g. an Epson "ET-2870 Series" printer). The enable hang was root-caused — the bridge
  must NOT be suspended across `BleIf_EnableBLE` (it starves the async `0x2A04` init-done
  event over the shared HIF) + the NWP needs Always-Active power mode.
- ✅ OTA receive → stage → install (STAGED) silicon-validated; bridge transport reliable.
- ⏳ **Production signing**: HSM step (key not on the bench).
- ✅ **OTA cold swap-boot — validated end-to-end on the EVK (2026-07-10).** A FORWARD
  candidate (version above the primary) → `state=2` STAGED → the CC35's own
  `psa_fwu_request_reboot()` swap (bridge drop+return) → the swapped image ran and
  **persisted across a true cold POR**. A first OTA after a failed one recovers cleanly
  (no bridge wedge, no CC35 reset — `ota_do_begin` stuck-slot recovery, #611). See
  `firmware/cc3501e/BRINGUP_STATUS.md` §5.
- ✅ **Wi-Fi + BLE CONCURRENT — validated on silicon (2026-06-24, E1M-AEN801 EVK).**
  `wifi scan`, `ble enable` (NimBLE host up), and `wifi connect` (WPA3, async) all
  **succeed together**, and the HW-CS bridge survives the combined radio load (a `ver`
  after a connect still returns — no desync, no power-cycle).  The earlier "Wi-Fi scan
  and BLE are NOT concurrent — use one radio at a time" limitation is **resolved**.

  Background: the prior block on a `Wlan_Scan` *while BLE is up* traced to the closed
  NWP firmware (**FW 1.8.0.42**) SoftGemini coexistence arbiter — TI issue
  **OSPREY_MX-1518** (`CMD_STATUS_REJECT_MEAS_SG_ACTIVE = 11`) — surfacing on the older
  CS-less r1 transport.  On the current hardware-SS0 transport (Alif `P14_7` =
  `SPI1_SS0_C`, dwc-ssi driving SS0 per transfer) with the worker-routed async path and
  the NimBLE-prio fix, the concurrent flow runs on the bench.  (Validated on the
  E1M-AEN801 EVK bench; not a production-certification claim.)
