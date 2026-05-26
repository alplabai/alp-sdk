# 0006. Secure boot + secure OTA

Status: Accepted, partially superseded (v0.4 delivery)
Date: 2026-05-10
Amended: 2026-05-11 (see "Amendment" section at the bottom)

## Context

ALP SDK targets connected, AI-enabled edge devices.  Every shipped
product needs:

1. **Secure boot** — the SoC verifies a cryptographic signature on
   the application image before it runs.  Stops attackers from
   booting modified firmware on stolen / cloned devices.
2. **Secure OTA** — over-the-air update channel that delivers
   signed firmware images, verifies them on-device, and either
   commits or rolls back atomically.  Indispensable once devices
   ship to customers; a recall is far more expensive than getting
   this right up front.

These are both v0.4 deliverables per `VERSIONS.md`.  This ADR locks
the design before code lands so the v0.4 cycle is unblocking work
rather than discovery.

The SDK has multiple SoM families with different boot semantics:

- **E1M-AEN family** (Alif Ensemble M55) — boots via Alif's
  on-chip Secure Enclave (an external boot ROM image-loader that
  understands signed manifests).  Application code can use
  MCUboot as a second-stage bootloader for OTA banking.
- **E1M-N93 family** (NXP i.MX 93) — boots via NXP's AHAB
  (Advanced High Assurance Boot) ROM.  Linux side runs U-Boot →
  kernel → user-space; MCUboot is irrelevant.
- **E1M-X V2N family** (Renesas RZ/V2N) — boots via Renesas'
  Secure Boot Manager (SBM).  Same Linux-side considerations as
  i.MX 93.

The SDK must abstract over these so app developers see one OTA
surface even though the underlying mechanisms differ.

## Decision

### Secure boot (per-SoM, vendor-native)

The SDK does NOT replace the vendor's secure-boot ROM.  We pin
configuration + tooling for each SoM:

| SoM family   | Bootloader        | Trust root              | Tooling                                                |
|--------------|-------------------|-------------------------|--------------------------------------------------------|
| E1M-AEN      | Alif Secure Enclave → MCUboot (2nd-stage) | OTP-burned Ed25519 public key | `vendors/alif/tools/sign.py` wraps Alif's signer; MCUboot uses `imgtool.py`. |
| E1M-N93      | NXP AHAB → U-Boot → kernel                 | OTP-burned SRK hash (SHA-256 of NXP key table) | `vendors/nxp/tools/cst.py` (NXP Code Signing Tool wrapper). |
| E1M-X V2N    | Renesas SBM → U-Boot → kernel              | OTP-burned RSA-2048 key digest | `vendors/renesas-rzv2n/tools/sbm-sign.py` wraps Renesas SBM signer. |

The OTP key provisioning step is one-shot per device and lives
**outside** the SDK build — it's a manufacturing step.  The SDK
ships:

- A `docs/secure-boot-provisioning.md` walkthrough per SoM for
  the first-time key burn.
- The per-vendor signer wrappers under
  `vendors/<vendor>/tools/sign.py` that take a built image and
  produce a signed image.
- A `west sign` hook on Zephyr targets so `west build && west sign
  && west flash` produces a flashable signed image without extra
  manual steps.

### Secure OTA (cross-SoM via `<alp/iot.h>`)

We extend the existing `<alp/iot.h>` surface with a small OTA
sub-surface:

```c
typedef struct alp_ota alp_ota_t;

typedef enum {
    ALP_OTA_TRANSPORT_MQTT  = 0,  /* image URL delivered via MQTT */
    ALP_OTA_TRANSPORT_HTTPS = 1,  /* image fetched from HTTPS URL */
} alp_ota_transport_t;

typedef struct {
    alp_ota_transport_t transport;
    const char         *manifest_url; /* HTTPS / MQTT topic */
    const uint8_t      *trust_anchor; /* DER-encoded cert pinning the manifest's signer */
    size_t              trust_anchor_len;
    uint32_t            min_version;  /* refuse to install older */
} alp_ota_config_t;

alp_ota_t   *alp_ota_open(const alp_ota_config_t *cfg);
alp_status_t alp_ota_check(alp_ota_t *ota, alp_ota_update_info_t *info_out);
alp_status_t alp_ota_apply(alp_ota_t *ota, const alp_ota_update_info_t *info,
                           alp_ota_progress_cb_t cb, void *user);
alp_status_t alp_ota_rollback(alp_ota_t *ota);
void         alp_ota_close(alp_ota_t *ota);
```

Internally:

- **Zephyr backends (AEN, N93-RTcore)**: route through MCUboot's
  swap-with-revert dual-bank flow.  `apply()` writes to the
  secondary slot, swaps, reboots; on first boot the new image
  must call `boot_write_img_confirmed()` (wrapped behind
  `alp_ota_commit()`) or the bootloader reverts on next reset.
- **Linux backends (N93-Linux, V2N, V2N-M1)**: route through
  RAUC (industry-standard A/B-banking update framework for
  embedded Linux).  `apply()` invokes `rauc install`; the
  bootloader environment tracks active slot and rollback.

### Threat model coverage

- **Image tampering**: signature verified before execution
  (vendor secure boot) AND before commit (MCUboot / RAUC).
- **Downgrade attacks**: `min_version` field in OTA config +
  bootloader version policy.
- **Server compromise**: trust-anchor pinning in
  `alp_ota_config_t.trust_anchor` rejects manifests not signed
  by the pinned cert chain.
- **MITM**: HTTPS transport uses MbedTLS PSA Crypto (already in
  `<alp/security.h>`) with the same trust anchor.
- **Battery exhaustion** (writing image then losing power):
  MCUboot's swap-with-revert and RAUC's A/B both leave the
  device bootable on the previous image if the new one is
  unconfirmed within a watchdog window.

### Out of scope (deferred to v1.x)

- **Anti-rollback fuses**: vendor-specific, requires OTP burns
  per release.  Documented as a manufacturing step but not
  automated by the SDK.
- **Encrypted manifests**: image confidentiality (vs integrity).
  Not commonly needed for OSS firmware; revisit if a customer
  demands it.
- **Delta updates**: byte-level diffing.  Cuts bandwidth but
  adds significant complexity; skip until field deployments
  prove bandwidth is the bottleneck.

## Consequences

**Positive:**

- App developers see one OTA API across Zephyr + Linux SoMs.
- Vendor-native secure boot keeps each SoM compatible with its
  upstream support story (Alif Secure Enclave updates,
  NXP AHAB advisories, etc.).
- MCUboot + RAUC are both battle-tested OSS — we're not
  shipping homegrown crypto.

**Negative:**

- Two backend implementations to maintain (MCUboot path,
  RAUC path).  Mitigation: each is a thin wrapper around the
  upstream tool; we don't fork either.
- First-time provisioning is per-SoM and is a manufacturing
  step the SDK can't fully automate.  Mitigation: per-SoM
  walkthroughs in `docs/secure-boot-provisioning.md`.

**Neutral:**

- The OTA API ships in `<alp/iot.h>` (extending it) rather than
  a new `<alp/ota.h>` -- OTA is fundamentally a network
  operation and keeping it adjacent to MQTT / HTTPS clients in
  the same header avoids splitting a small surface.

## Roadmap (v0.4 cycle)

| Deliverable                                | Where it lands                                         |
|--------------------------------------------|--------------------------------------------------------|
| ADR 0006 (this)                            | `docs/adr/0006-secure-boot-secure-ota.md` -- ✅ landed |
| `alp_ota_*` surface in `<alp/iot.h>`       | `include/alp/iot.h` -- v0.4                            |
| MCUboot integration (AEN, N93-RTcore)      | `src/zephyr/ota_mcuboot.c` -- v0.4                     |
| RAUC integration (N93-Linux, V2N, V2N-M1)  | `src/yocto/ota_rauc.cpp` + RAUC config in meta-alp-sdk -- v0.4 |
| Per-SoM signing wrappers                   | `vendors/<vendor>/tools/sign.py` -- v0.4               |
| `west sign` hook                           | Zephyr module hook -- v0.4                             |
| Provisioning walkthrough                   | `docs/secure-boot-provisioning.md` -- v0.4             |
| OTA bring-up example                       | `examples/iot-ota-aen/` -- v0.4                        |

## Amendment (2026-05-11)

The v0.4-prep work has diverged from the original ADR on two
points.  This section is the authoritative current direction; the
sections above remain for the audit trail.

**1. Linux OTA agent: Mender, not RAUC.**

The original decision picked RAUC for the Linux side.  v0.4-prep
landed Mender wiring instead, via
[`meta-alp-sdk/conf/distro/include/mender.inc`](../../meta-alp-sdk/conf/distro/include/mender.inc).
Rationale for the switch:

- Mender's hosted server + on-target client are mature and
  well-documented; RAUC's reference server (Hawkbit-via-RAUC) is
  thinner.
- A separate ALP-owned OTA-server project (in another repo) is
  planned -- starting Mender-protocol-compatible keeps the device
  side unchanged when that server replaces the hosted Mender
  instance.
- Mender's swap semantics (A/B rootfs + U-Boot integration +
  commit health-check) are essentially identical to RAUC's; no
  feature loss on the switch.

**2. AEN-Zephyr OTA client: decision pending.**

The original ADR commits AEN-Zephyr to MCUboot's swap-with-revert
flow.  MCUboot scaffolding has landed
([`zephyr/sysbuild/aen/sysbuild.conf`](../../zephyr/sysbuild/aen/sysbuild.conf) +
[`docs/secure-boot.md`](../secure-boot.md)).  The OTA-delivery
half (Mender Zephyr client vs Hawkbit-on-Zephyr) is **decision
pending** for v0.4-final -- see
[`docs/ota.md`](../ota.md) for the two-option analysis.

**3. `alp_ota_*` API not declared yet.**

The ADR specified a new sub-surface in `<alp/iot.h>`
(`alp_ota_open` / `_check` / `_apply` / `_rollback`).  This
**hasn't shipped** as of 2026-05-11.  The current Yocto-side
delivery vehicle is plain Mender (operators interact with
`mender-client` / `mender-connect` directly on the device); the
v0.4-final cycle decides whether to add the `alp_ota_*` wrapper
on top, or treat Mender's client API as the public surface
directly.

**4. Cross-cutting OTA doc.**

The original ADR didn't anticipate
[`docs/ota.md`](../ota.md), which now carries the trust-model +
flow + decision-pending notes for both backends.  That doc is the
operator-facing reference; this ADR is the historical decision
record.

## See also

- [ADR 0001](0001-wrapper-on-top-of-zephyr.md) -- the layering
  philosophy that keeps vendor-native bootloaders intact.
- [ADR 0002](0002-error-mechanism.md) -- `alp_last_error()` is
  the diagnostic channel for OTA failures.
- [ADR 0005](0005-alp-sdk-vs-alp-studio-boundary.md) -- secure
  boot keys are NEVER in either repo; provisioning lives in the
  manufacturing flow (out of scope for both).
- [`VERSIONS.md`](../../VERSIONS.md) -- the v0.4 milestone
  carries the actual delivery.
- [`docs/secure-boot.md`](../secure-boot.md) -- current MCUboot
  chain-of-trust + key lifecycle for AEN-Zephyr.
- [`docs/ota.md`](../ota.md) -- current OTA story (Yocto Mender
  flow + AEN-Zephyr decision pending).
- [MCUboot project](https://docs.mcuboot.com/) -- upstream
  reference for the Zephyr-side flow.
- [Mender project](https://mender.io/) -- upstream reference for
  the Linux-side flow (substituted for RAUC per the Amendment).
