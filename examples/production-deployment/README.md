# production-deployment

The v1.0 reference application for a field-deployable IoT
product.  Demonstrates the full manufactured → deployed →
updated → attested lifecycle on a single app, so customers see
how the SDK's secure-boot, OTA, EEPROM-provisioning, and
remote-attestation pieces fit together.

## What this shows

This is the SDK's *integration* flagship -- every other example
covers one library surface; this one covers the production
lifecycle that ties them all together.

### Stage 1: factory-provisioning read-back

On boot, `<alp/hw_info.h>` reads the manufacturer EEPROM
manifest programmed at factory test.  The manifest carries the
board's per-unit identity (SKU, serial, HW revision, factory
date).  Production firmware treats this as authoritative --
never derive identity from the SoC's internal UID alone.

### Stage 2: secure-boot evidence

MCUboot has already validated the running image against the
ECDSA-P256 boot key by the time `main()` runs (the bootloader
rejects unsigned / wrong-key images at boot).  The app reads
back the active slot + image revision so a cloud-side fleet
console can confirm the deployed firmware matches what the
signing service produced.  Signing-service contract lives in
[`docs/secure-boot.md`](../../docs/secure-boot.md).

### Stage 3: OTA polling + apply

`<alp/iot.h>` opens a TLS-mutual-auth connection to a Mender
server and polls for a deployment.  When one is pending, the
SDK downloads the new image, verifies its signature against the
same ECDSA-P256 boot key, writes it to the inactive MCUboot
slot, sets the swap-pending flag, and requests a reboot.
Post-reboot, MCUboot confirms or reverts based on the new
image's `mark_confirmed` call.

### Stage 4: remote attestation

A cloud-side fleet operator needs evidence that the device
hasn't been physically tampered with -- per the threat model
([`docs/threat-model.md`](../../docs/threat-model.md) §asset 8).
The app publishes a periodic heartbeat that includes the
OPTIGA-signed boot log + the current secure-boot slot.  An
attacker who swaps the flash sees the OPTIGA signature break.

## Build

### native_sim (framing test, no real ops)

```bash
west build -b native_sim/native/64 examples/production-deployment
west build -t run
```

Expected output:

```
[prod] alp-sdk production-deployment flagship
[prod] stage 1: reading manufacturer EEPROM manifest
[prod]   alp_hw_info_read -> -2 -- continuing with no identity
[prod] stage 2: reading MCUboot slot info
[prod]   internal flash open -> last_err=-2
[prod] stage 3: connecting to Mender server
[prod]   wifi open -> NOSUPPORT (native_sim) or NOT_READY (HiL pre-DT)
[prod]   attestation: 64-byte nonce drawn from TRNG
[prod] done
```

### Real silicon (AEN-Zephyr, requires a staged Mender server)

```bash
west alp-build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/production-deployment
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

- Fork this skeleton for V2N or i.MX 93 boards (the SDK's
  portable surfaces stay identical; only `board.yaml` changes).
- Replace the Mender connection with a different OTA fabric
  (azure-iot-hub, AWS IoT Core, custom HTTPS endpoint).  The
  `<alp/storage.h>` slot-write code path stays the same.
- Add domain-specific business logic between the OTA poll +
  the attestation heartbeat.

The skeleton is structured so each stage is one short function
in `src/main.c`; customers wire their domain logic alongside
without rewriting the SDK glue.

## Reference

- [`<alp/hw_info.h>`](../../include/alp/hw_info.h) -- factory
  EEPROM manifest read-back.
- [`<alp/storage.h>`](../../include/alp/storage.h) -- MCUboot
  slot inspection + OTA chunk write.
- [`<alp/iot.h>`](../../include/alp/iot.h) -- Wi-Fi + MQTT +
  TLS for the Mender connection.
- [`<alp/security.h>`](../../include/alp/security.h) -- AEAD
  + TRNG primitives for attestation.
- [`docs/secure-boot.md`](../../docs/secure-boot.md) -- the
  ECDSA-P256 signing-service contract.
- [`docs/threat-model.md`](../../docs/threat-model.md) §asset 8
  -- the tamper-evidence requirement.
- [`docs/tutorials/12-mender-ota.md`](../../docs/tutorials/12-mender-ota.md)
  -- step-by-step walkthrough of the Mender path.
- [`docs/v1.0-readiness.md`](../../docs/v1.0-readiness.md) §4
  -- this example is the production-deployment flagship.
