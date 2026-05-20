# OTA on ALP SDK

Secure over-the-air firmware update is a v0.4 deliverable for
every supported SoM family.  The mechanism differs per OS
backend but the trust story and the operator-facing flow are
shared: signed artefact uploaded to a server, signed artefact
delivered to the device, image swap behind an atomic rollback
guarantee.

> **Status: v0.4-prep.**  Yocto-side Mender wiring scaffolding
> landed in this revision (opt-in via `require
> conf/distro/include/mender.inc` in a machine .conf).
> AEN-Zephyr side is documentation-only -- Mender's Zephyr
> client choice is still open and lands alongside the v0.4
> AEN secure-boot integration.

## Trust model

The signing keys behind OTA are the **same keys** that gate
secure boot:

- AEN-Zephyr: MCUboot ECDSA-P256 (see [`docs/secure-boot.md`](secure-boot.md)).
- Yocto (V2N / V2N-M1 / i.MX 93): per-rootfs ECDSA signature
  verified by U-Boot before MCUboot-style A/B swap.

A signed artefact accepted as a boot image is therefore also
accepted as an OTA payload, and vice versa.  This is by design --
fewer keys, fewer surfaces, fewer mistakes.

## Yocto path: Mender

### Declarative wiring (`ota:` block in `board.yaml`)

The recommended path is the top-level `ota:` block in your
project's `board.yaml` -- the loader (`scripts/alp_orchestrate.py`)
emits the matching `MENDER_*` weak-assignments + `INHERIT +=
"mender-full"` into the slice's generated `local.conf`.  No
hand-edited Mender variables.

```yaml
# board.yaml
ota:
  provider: mender
  artifact_name: my-product-1.2.3
  signing_key: keys/mender_artifact.pem
  server:
    url: https://hosted.mender.io
    tenant: my-tenant
  rollback: { enabled: true, retries: 3, min_version: 1 }
  poll_interval_s: 1800
  storage:
    device: /dev/mmcblk0p
    boot_part_mb: 64
    rootfs_ab: true
    total_size_mb: 4096
```

See [`docs/board-config.md` §OTA](board-config.md#ota-ota----mender--mcumgr)
for the full field reference.  The emitted `local.conf` uses `?=`
(weak) assignments so hand-edits in the build directory still win
if a developer wants to override per-tree.

### Underlying layer

The orchestrator output drives
[`meta-alp-sdk/conf/distro/include/mender.inc`](../meta-alp-sdk/conf/distro/include/mender.inc),
which `meta-alp-sdk` pulls in via the generated `INHERIT +=` line.
See the [`meta-alp-sdk` README](../meta-alp-sdk/README.md#ota-via-mender)
for the layer-level walk-through if you're working below the
`board.yaml` surface.

Reference flow on a Mender-enabled image:

```
┌──────────────────────────────────────────────────────────────┐
│ Build host                                                   │
│   bitbake core-image-weston                                  │
│     -> rootfs-${MACHINE}.wic.gz       (first-boot flash)     │
│     -> rootfs-${MACHINE}.mender       (signed OTA artefact)  │
├──────────────────────────────────────────────────────────────┤
│ Mender server (hosted or self-hosted)                        │
│   - Stores .mender artefacts.                                │
│   - Schedules deployments to device groups.                  │
│   - Tracks per-device install + commit + rollback events.    │
├──────────────────────────────────────────────────────────────┤
│ Device                                                       │
│   mender-client polls / receives push.                       │
│   - Downloads the .mender artefact.                          │
│   - Verifies the signature against the public key in the     │
│     image's /etc/mender/server.crt (provisioned at build).   │
│   - Writes to the inactive A/B rootfs partition.             │
│   - Sets the U-Boot env to boot the new slot on next reboot. │
│   - Reboots.                                                 │
│ On boot:                                                     │
│   - U-Boot boots the new slot with a one-shot flag.          │
│   - Boot health check runs (default: 30 s grace + connect    │
│     to server + commit).                                     │
│   - If commit lands: U-Boot env flips permanently to new.    │
│   - If commit does NOT land (crash / hang / no network):     │
│     next reboot falls back to the old slot automatically.    │
└──────────────────────────────────────────────────────────────┘
```

Failure modes covered:

| Failure                          | Recovery                                   |
|----------------------------------|--------------------------------------------|
| Signature mismatch on download   | Artefact rejected; no partition write.     |
| Power loss mid-write             | New slot half-written; old slot intact;    |
|                                  | next boot uses old slot.                   |
| Power loss between write + reboot| One-shot U-Boot flag points at new slot;   |
|                                  | new slot boots; commit logic decides.      |
| New slot crashes before commit   | U-Boot fallback on next boot.              |
| New slot stable, no server conn  | mender-client retries until commit window  |
|                                  | expires; falls back automatically.         |

## AEN-Zephyr path: MCUboot + Mender (v0.4 plan)

AEN's MCUboot scaffolding (see [`docs/secure-boot.md`](secure-boot.md))
sets up the signed-image verification half.  The delivery half
is open between two viable options:

### Option A — Mender Zephyr client (preferred)

Mender's Zephyr support is upstream as the
[`mender-mcu-client`](https://github.com/mendersoftware/mender-mcu-client)
library (Apache-2.0).  It targets Zephyr v3.6+ + MCUboot + LwM2M
or HTTPs transport; the v0.4-final integration:

1. Pulls `mender-mcu-client` in via a west group.
2. Wires it onto `<alp/iot.h>`'s MQTT/HTTPS transport on
   AEN-Zephyr (TLS path lands alongside `<alp/security.h>`
   PSA crypto).
3. Wires its image-write hook onto MCUboot's secondary slot.

Caveats:

- The Zephyr client API is younger than the Linux client; some
  features (per-update inventory blob, deployment retry policy)
  are still labelled experimental upstream.
- Footprint: ~50 KiB additional flash + ~20 KiB RAM on the
  configurations the SDK targets.

### Option B — Hawkbit + bespoke wire

Eclipse Hawkbit is a Mender-alternative deployment server with a
mature Zephyr client ([`zephyr/subsys/mgmt/hawkbit`](https://docs.zephyrproject.org/latest/services/device_mgmt/hawkbit.html)).
Trade-off: smaller footprint, more bespoke server-side ops.
Picked only if Mender's Zephyr client misses a hard requirement.

### Decision pending

The v0.4 release picks one of the two before tagging.  Until
then this section is the contract -- alp-sdk does not commit to
a Zephyr-side OTA client API in the public `<alp/...>` headers;
the chosen client's API is what apps use directly.

## CC3501E's role in OTA delivery on AEN

The AEN SoM's Wi-Fi/BLE bridge is the
[`TI CC3501E`](cc3501e-bridge.md).  OTA payloads land on the
Alif Ensemble main SoC through the inter-chip SPI1 bus, not
the CC3501E's own flash.  The CC3501E firmware is **not** updated
via the same OTA path -- it ships with a separate upgrade flow
described in the CC3501E firmware repo
([`alplabai/cc3501e-firmware`](https://github.com/alplabai/cc3501e-firmware)).
Decoupling the two firmwares means a failed CC3501E update can't
brick the Alif side and vice versa.

## See also

- [`docs/secure-boot.md`](secure-boot.md) — the secure-boot half
  of the chain (key lifecycle, signing flow).
- [`meta-alp-sdk/README.md`](../meta-alp-sdk/README.md) —
  the Yocto layer's Mender integration walk-through.
- [`docs/cc3501e-bridge.md`](cc3501e-bridge.md) — Wi-Fi/BLE
  bridge architecture; relevant for the network transport of
  OTA payloads on AEN.
- [`VERSIONS.md`](../VERSIONS.md) — versioned roadmap; secure
  boot / OTA shipped together in v0.4.
