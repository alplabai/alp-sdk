# Device-side OTA contract

This document captures the **device-side** half of the OTA contract
for the E1M-X family.  The **server-side** is owned by a separate
team and lives in a separate repository (not part of this public
SDK).  The interface between the two halves is the device-side
Mender contract documented below + the shared trust model in
[`docs/secure-boot.md`](secure-boot.md).

Two layers of OTA exist:

1. **Main system OTA** -- Mender on the Renesas RZ/V2N (or Alif
   E7 / NXP i.MX 93 on other E1M variants).  Updates the Linux
   kernel, root filesystem, and userspace.  This is the "big"
   OTA.
2. **GD32 bridge firmware OTA** -- Path A (application bootloader
   over the bridge) implemented and silicon-validated, gated behind
   `-DBRIDGE_OTA_PARTITIONED` (HIL-pending); opcodes `0xF0..0xFF`
   and the wire contract are in
   [`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md) §10.

The two paths are independent.  Mender does not flash the GD32;
the GD32 application bootloader does.

## Main system OTA (Mender)

### Contract

The server side (Hakan's repo) hands the device an artifact
(`.mender` tarball) over HTTPS.  The device must:

1. **Authenticate.** The server-provided `.mender` artifact must be
   signed with an ECDSA-P256 key whose signing half is held in the
   build/sign side's OPTIGA Trust M (see
   [`docs/secure-boot.md`](secure-boot.md) for the trust model).  On
   the device, the verification anchor is the matching **Mender
   artifact-verify public key provisioned into the rootfs** (the
   `/etc/mender/` verify key / `server.crt` written at build time) —
   the device does *not* invoke a secure element for per-artifact
   verification.
2. **Stage.** Mender writes the new root-FS image to the inactive
   A/B slot on eMMC.  The active slot keeps running.
3. **Verify.** The mender-client verifies the artifact's ECDSA-P256
   signature against that provisioned rootfs public key (the same
   anchor as [`docs/ota.md`](ota.md)) and refuses to commit on a
   mismatch.
4. **Commit + reset.** Mender flips the bootloader's A/B selector,
   issues a reset.
5. **Confirm health post-reset.** Within `MENDER_INVENTORY_INTERVAL`
   (default 60 s) the device confirms it boots, contacts the
   server, and acknowledges the new version.  If the device
   doesn't confirm within `MENDER_RETRY_POLL` (default 5 min), the
   bootloader rolls back to the previous slot on the next reset.

### Failure modes

| Failure                                  | Device response                                                             |
|------------------------------------------|-----------------------------------------------------------------------------|
| Server-signed artifact has bad signature | Mender aborts, leaves active slot untouched, reports failure to server.     |
| Staged slot fails SHA-256                | Same.                                                                       |
| Server unreachable mid-stage             | Mender retries with exponential back-off.  No state change on the device.   |
| Power-loss mid-stage                     | Inactive slot is corrupt but never marked bootable; active slot still runs. |
| New slot boots but health-check fails    | Bootloader rolls back on next reset (Mender's standard committed-rollback). |

### What the device runtime exposes to Mender

| Surface                          | Notes                                                            |
|----------------------------------|------------------------------------------------------------------|
| `/etc/mender/mender.conf`        | Yocto-managed; points at the server URL.                          |
| OPTIGA Trust M key handle        | The signing-cert is provisioned at factory; runtime fetches via `<alp/security.h>`. |
| `alp_hw_info_read()`             | Mender's inventory uses the SoM family / SKU / hw_rev / serial.   |
| `/etc/mender/artifact_info`      | Yocto-emitted artifact-name string ("v2n-r1-26W19-build42").       |

### Cross-link

* Mender server (Hakan's repo) -- URL TBD; do not propose
  server-side code here.
* OPTIGA Trust M driver: [`chips/optiga_trust_m/`](../chips/optiga_trust_m/).
* `<alp/security.h>` -- runtime crypto surface that Mender uses
  for signature verification.

## GD32 bridge firmware OTA (planned)

### Status

**Path A implemented (gated, HIL-pending); Path B scaffolded.**
The `0xF0..0xFF` opcode set is implemented in
`firmware/gd32-bridge/src/ota.c` and specified in
[`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md) §10;
destructive flashing is armed only in `-DBRIDGE_OTA_PARTITIONED`
builds (the default build answers `STATUS_NOSUPPORT`).  Host-driven
SWD via the V2N programming header remains the universal recovery
path (Path B).

### Opcode set

The authoritative wire contract is
[`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md) §10
(implemented in `firmware/gd32-bridge/src/ota.c`, mirrored host-side
in `<alp/chips/gd32g553.h>`).  As implemented, `OTA_BEGIN` carries
`size:u32 expected_crc32:u32` (plus an optional additive version
triple), session state is implicit rather than slot-addressed on
every op, and integrity is a CRC-32 (`OTA_VERIFY` returns
`computed_crc32:u32 verified:u8`).  See §10 for the full table
including `0xF6 OTA_ABORT`.

### Planned flash layout (GD32G553, 512 KB)

```
0x08000000  ┌──────────────────────────────────────────────┐
            │  Application bootloader (NEVER overwritten)  │   N KiB
0x08000000  │                                              │
+ N KiB     ├──────────────────────────────────────────────┤
            │  Slot A (application image)                  │   (512 - N) / 2 KiB
            │                                              │
            ├──────────────────────────────────────────────┤
            │  Slot B (application image)                  │   (512 - N) / 2 KiB
            │                                              │
            ├──────────────────────────────────────────────┤
            │  Metadata page (active-slot, version, sha)   │   < 1 KiB
0x08080000  └──────────────────────────────────────────────┘
```

Exact `N` TBD pending bootloader implementation; ~32 KiB is a
reasonable starting estimate for a Cortex-M33 bootloader.

### Crypto

GD32 bridge firmware images carry:

* Truncated SHA-256 (full hash, not truncated; the "truncated"
  refers to the build-id macro which is 20 ASCII hex chars =
  10 bytes of hash).
* ECDSA-P256 signature over the SHA-256 + version metadata,
  chaining to the OPTIGA Trust M's trust root.

The bridge bootloader verifies the signature **on the GD32 side**
(no OPTIGA available -- the GD32 doesn't have I2C to the OPTIGA in
the current schematic).  This means the public key is **baked
into the bootloader image** at compile time, and rotating the
signing key requires a bootloader update (SWD).

A future revision may add an OPTIGA-mediated verification path,
but that's a post-1.0 problem.

## Failure-recovery matrix (combined)

| Scenario                                                       | Recovery path                                                                 |
|----------------------------------------------------------------|-------------------------------------------------------------------------------|
| Main-system OTA bricks userspace                               | Bootloader rolls back to the previous A/B slot.                              |
| Main-system OTA corrupts the kernel image                      | Same.                                                                         |
| Main-system OTA corrupts the bootloader (eMMC partition table) | SWD recovery; flash a new bootloader.                                         |
| GD32 OTA bricks the application slot                           | Application bootloader runs the other slot.                                  |
| GD32 OTA bricks both application slots                         | SWD recovery on the GD32; or, post-`BOOT0`-wiring, V2N-driven factory ISP.    |
| GD32 application bootloader itself is corrupt                  | SWD only.  Until / unless `BOOT0` is wired to V2N + a factory-ISP host flow lands. |

## Open questions

* Final layout of the GD32 bootloader (32 KiB vs 16 KiB)?
* Signing key rotation strategy for the GD32 bootloader (key in
  ROM, key in flash, or key in OPTIGA via a small SPI/I2C cross-bus)?
* Mender artifact-name format -- align across V2N + V2N-M1 + AEN?

## See also

* [`docs/ota.md`](ota.md) -- v0.3 OTA design (high-level).
* [`docs/secure-boot.md`](secure-boot.md) -- trust model + signing.
* [`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md) §10 --
  GD32 bootloader-OTA opcode reservation.
* [`chips/optiga_trust_m/`](../chips/optiga_trust_m/) -- OPTIGA
  driver used for main-system signature verification.
