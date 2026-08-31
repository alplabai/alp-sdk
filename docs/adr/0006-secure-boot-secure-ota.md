# 0006. Secure boot + secure OTA

Status: Accepted, partially superseded (v0.4 delivery)
Date: 2026-05-10
Amended: 2026-05-11, 2026-07-31, 2026-08-25 (see "Amendment" sections at the bottom)

## Context

Alp SDK targets connected, AI-enabled edge devices.  Every shipped
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
  **This was the design intent; it is not what the AEN family
  ships -- see the "Amendment (2026-08-25)" section at the bottom.**
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
- A separate Alp-owned OTA-server project (in another repo) is
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

## Amendment (2026-07-31)

MCUboot's role in the AEN secure-boot chain (§ "Secure boot (per-SoM,
vendor-native)" above) is now bench-proven rather than aspirational.

**5. SES → MCUboot → slot0 verification is bench-proven.**

On E1M-AEN801 (`AE822FA0E5597LS0` Rev A0, alp-sdk `0da1f1b4`) the chain
SES -> MCUboot (ITCM) -> slot0 (MRAM XIP) -> application boots with
`CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y` and
`CONFIG_BOOT_VALIDATE_SLOT0=y` (read back from the built
`mcuboot/zephyr/.config`, not assumed).  Verification was proven live:
flipping one byte of the TLV `0x22` ECDSA signature (file offset
`0x4a30`, `0xda` -> `0xdb`) produces `bootutil_verify_sig: ECDSA
builtin key 0` then `Unable to find bootable image` -- the failure
path runs to completion rather than hanging.  Three previously
recorded symptoms -- the software ECDSA-P256 verify hang at that same
`bootutil_verify_sig` log line, the SES → MCUboot → slot0 chain
failing to complete, and the resulting "not shippable for this part"
verdict -- no longer reproduce at `0da1f1b4` (all three cleared
together with no crypto/SHA/flash-driver change, which is consistent
with -- but has not been confirmed by bisection against -- the MPU
work in commit `a6ff095d` / PR #1014; causation is not established).
A separate, two-slot swap-using-scratch build boots and logs
`Bootloader chainload address offset: 0x10000` -- boot only; the
swap/rollback path itself remains `[UNTESTED]`.  The verified backend
is TinyCrypt (`CONFIG_BOOT_ECDSA_TINYCRYPT=y`), not MbedTLS PSA; the
`.config` also confirms `CONFIG_SINGLE_APPLICATION_SLOT=y` and
`CONFIG_FLASH_BASE_ADDRESS=0x0`.  Still required: `CONFIG_DCACHE=n`,
the board's `ROM_START_OFFSET=0x800`, and the
`zephyr/patches/mcuboot` `do_boot` flash-base patch (a candidate for
upstreaming rather than carrying indefinitely).

**6. Customer production path is now bench-proven too.**

The above proves the *factory* path (Alp Lab's pre-provisioned MCUboot
+ signed app).  A second bench session proved the *customer* path as
well: writing slot0 with a plain J-Link -- no ATOC, no SE-UART -- is
verified by MCUboot and chainloaded, and survived three cold
power-cycles.  **Proven at `0x80010000` (slot0) only** -- the ATOC
region and erasing MCUboot itself were not tested.  Both refusal
shapes (tampered signature, non-MCUboot image) leave the debug port
alive, so a bad slot0 write does not brick J-Link access.  This is a
**single-slot** result (`CONFIG_SINGLE_APPLICATION_SLOT=y`); A/B swap
and OTA remain untested and this is not an upgrade-path guarantee.
See [`docs/aen-provisioning.md`](../aen-provisioning.md) §0.5 and
[`docs/secure-boot.md`](../secure-boot.md).

## Amendment (2026-08-25)

**7. The AEN family ships single-slot; swap-with-revert is not enabled.**

The "Secure OTA" decision above commits AEN-Zephyr to MCUboot's
swap-with-revert dual-bank flow -- a secondary slot to write into, a
swap, and a revert path if the new image never confirms.  Amendment 6
(2026-07-31) already flagged this as untested; it is now known to be
more than untested -- **the hardware configuration this SDK ships for
the AEN family does not have the partitions swap-with-revert needs.**

`1ad76193` (#1100, closing #1069, 2026-08-03) removed both
`slot1_partition` and `scratch_partition` from AEN801's MRAM map on
purpose, to fit the disjoint dual-core slot0 budget: the address that
was slot1/OTA (`0x802b0000`) became the HP core's slot0, and the
address that was scratch (`0x80550000`) became `reserved`.  #1445
later applied the same disjoint-per-core-slot0 trade to every other
dual-core AEN SoM, so this is not an AEN801 peculiarity.  The current
map for the HE core (`zephyr/boards/alp/e1m_aen801_m55_he/
alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.dts`, confirmed on
`origin/dev`):

```
boot_partition     @ 0x0        64 KiB   label "mcuboot"
slot0_partition    @ 0x10000  2688 KiB   label "image-0"
reserved_partition @ 0x550000   64 KiB   label "reserved"
storage_partition  @ 0x560000   96 KiB   label "storage"
atoc_partition     @ 0x578000   32 KiB   label "atoc"
```

The HP core's DT (`alp_e1m_aen801_m55_hp_ae822fa0e5597ls0_rtss_hp.dts`)
carries the same five partitions at the same sizes, only with its own
`slot0_partition` at `0x2b0000` (the address that used to be slot1) --
neither core's DT has a `slot1_partition` or `scratch_partition`.  Nor
do `e1m_aen401_m55_hp` or `e1m_aen601_m55_hp` -- confirmed against
`origin/dev`, `grep -rn 'slot1_partition\|scratch_partition'
zephyr/boards/alp/` returns nothing tree-wide.  Every board directory
under `zephyr/boards/alp/e1m_aen*` (`e1m_aen401_m55_hp`,
`e1m_aen601_m55_hp`, `e1m_aen801_m55_he`, `e1m_aen801_m55_hp`) carries
the same five-partition, no-slot1/no-scratch map.
`zephyr/sysbuild/aen/sysbuild.conf` records the same trade in its own
comments and sets `SB_CONFIG_MCUBOOT_MODE_SINGLE_APP`;
`examples/aen/aen-mcuboot-smoke/` carries an explicit "NO A/B YET"
note.

**What this means for the Decision section above:** swap-with-revert
remains the design intent for the AEN-Zephyr backend in general, and
nothing here reverses that intent.  But across the AEN family there
is currently no secondary slot to swap into and no scratch area to
swap through -- there is nothing to revert *to*.  A bad image on any
AEN target is not automatically rolled back today.  OTA on the AEN
family was deferred as part of the #1069/#1100 trade (later confirmed
family-wide by #1445) and stays deferred until a slot budget (or a
different delivery shape) is chosen -- tracked by #1066.  #1066 root-
caused why a swapped board on this MRAM would have re-swapped forever:
the step that removes the temporary scratch trailer is
`boot_erase_region()`, which on a device reporting no-erase semantics is
a **pure no-op** -- it logs `boot_erase_region: device without erase` and
returns 0 -- so the GOOD magic `swap_status_init()` wrote at the end of
scratch survives every swap and forces a resume on the next boot,
regardless of `image_ok`.  The fix is carried in `zephyr/patches.yml`
(`mcuboot/0004-scramble-scratch-trailer-...`), regression-tested on the
mcuboot host simulator (`mcuboot/0005-sim-...`) but **not bench-verified
on AEN silicon**: no AEN board has a scratch/slot1 partition to exercise
it against.

This paragraph used to attribute that loop to MCUboot's own
`copy_size == 0` degenerating the scratch-algorithm loop, and to the
`mcuboot/0002-bootutil_misc-...` patch as its fix.  **That attribution
was wrong** and is corrected here.  `swap_status_init()` writes
`swap_size` *before* it writes magic, so a GOOD scratch magic implies a
genuinely written, nonzero `swap_size`; the state that story needs --
GOOD magic together with an erased `swap_size` -- cannot arise from an
interrupted swap.  Patch `0002` stays: it closes a real, reproduced
crash on the `find_swap_count()` / `boot_read_image_header()` path,
where an erased-value `swap_size` on a `0x00`-erase device is
indistinguishable from a genuine `0`.  It simply never explained, or
fixed, the re-swap loop.  #1066's second finding -- a clean,
valid PERM upgrade request that produced no swap at all -- was never
root-caused and stays open.  Re-enabling OTA still needs a slot budget
chosen and the whole swap path (both findings) proven on real silicon
before the "with revert" claim can be restored here.

Whether to find AEN-family slot budget for OTA, ship it single-slot
permanently, or revisit the partition trade some other way is a
product decision this ADR does not make.

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
