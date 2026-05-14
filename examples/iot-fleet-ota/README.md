# iot-fleet-ota [UNTESTED]

Secure OTA firmware update with rollback. The production-readiness
proof for "how do we update 10 000 units in the field?".

Targets every E1M-X SoM family. native_sim build verified; HiL
verification gates on a staged Mender server (separate repo).

## Trust model

Four pieces fit together:

- **OPTIGA Trust M** -- on-SoM secure element. Holds the
  ECDSA-P256 private key in tamper-resistant NVM (slot 0xE0F0).
  The private half is generated *inside* the chip at SoM-mfg
  time and never leaves. Without physical access to a
  provisioned OPTIGA, no attacker can produce a signature this
  device will accept. See [`docs/secure-boot.md`](../../docs/secure-boot.md).
- **ECDSA-P256** -- the signing algorithm. The public half is
  read out of the OPTIGA once at provisioning, signed by the
  manufacturing CA, and compiled into the MCUboot bootloader.
  Same key gates both secure boot and OTA acceptance -- one
  trust root, fewer surfaces.
- **MCUboot slot-A/B** -- two-slot firmware layout with
  swap-using-scratch. The new image lands in the inactive slot;
  MCUboot re-verifies its signature at boot, swaps, hands off.
  Mid-swap power loss recovers atomically via the scratch
  sector.
- **Mender protocol** -- HTTPS-poll deployment fabric. The
  device GETs the server's `/deployments/next` endpoint every
  60 s; the server answers either "no deployment" or a
  signed-artefact manifest.

## Rollback semantics

The new image must call `boot_set_confirmed()` within its
health-check window (typically 30 s after boot, after a
successful Mender check-in). If it doesn't -- because it
crashes, hangs, or can't reach the server -- MCUboot's "test
pending" flag triggers an automatic rollback to the previous
slot on the next reboot. A bricking OTA undoes itself: the
watchdog-friendly safety net the fleet operator needs to sleep
at night.

## Server side

The Mender server lives in a separate repo (owned by Hakan).
This demo is the device side only. Customers point
`MENDER_SERVER_URL` in `src/main.c` at their own Mender
instance.

## References

- [`docs/secure-boot.md`](../../docs/secure-boot.md) -- chain of
  trust, signing key lifecycle, key rotation playbook.
- [`docs/ota.md`](../../docs/ota.md) -- Mender Zephyr client
  option (Option A) and the open delivery-half decision.
- [`docs/cc3501e-bridge.md`](../../docs/cc3501e-bridge.md) --
  Wi-Fi transport on AEN.
- [`examples/production-deployment`](../production-deployment/) --
  the broader lifecycle (factory ID + attestation + OTA in one app).
