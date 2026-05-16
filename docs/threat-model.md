# Threat model — ALP SDK consumer surface

A v1.0-prep threat model for the consumer-facing surface of the
ALP SDK.  Identifies trust boundaries, asset classes, primary
adversaries, and the mitigation we ship against each.

> **Scope.** This doc covers the SDK's public surface (`<alp/*.h>`,
> `board.yaml`, the loader, the chip drivers we ship).  Out of
> scope: vendor-SDK CVEs we wrap (Alif / Renesas / NXP / DEEPX /
> TI) — those track upstream.  Application code built on top is
> also out of scope; threats specific to a customer's app belong
> in that customer's threat model.
>
> **Companion docs.** [`docs/secure-boot.md`](secure-boot.md)
> covers the trust model + key lifecycle.
> [`docs/security-advisories.md`](security-advisories.md) is the
> disclosure workflow.  This doc is the *systematic* enumeration
> the audit reviewer + the external security firm work against.

## 1. Trust boundaries

```
        ┌─────────────────────────────────────────────────────┐
        │  Application code (customer-owned, untrusted-to-SDK)│
        └──────────────────────┬──────────────────────────────┘
                               │  <alp/*.h> API
        ┌──────────────────────▼──────────────────────────────┐
        │  ALP SDK -- public surface + loader (TRUSTED)        │
        │  Schema validation, capability checks, last-error    │
        └──────────────────────┬──────────────────────────────┘
                               │  OS-pivoted backend dispatch
        ┌──────────────────────▼──────────────────────────────┐
        │  src/{zephyr,baremetal,yocto}/* + chips/*           │
        │  Vendor-HAL wrappers + chip drivers (TRUSTED)        │
        └──────────────────────┬──────────────────────────────┘
                               │  Vendor SDK / OS kernel
        ┌──────────────────────▼──────────────────────────────┐
        │  Alif / Renesas / NXP / TI SDKs + Zephyr / Linux    │
        │  (semi-trusted -- upstream CVE policy applies)       │
        └──────────────────────┬──────────────────────────────┘
                               │  ARM CMSIS / silicon registers
        ┌──────────────────────▼──────────────────────────────┐
        │  Silicon (root of trust: OPTIGA Trust M + MCUboot)   │
        └─────────────────────────────────────────────────────┘
                               ▲
              ┌────────────────┴──────────────────┐
              │                                   │
       ┌──────▼───────┐                  ┌────────▼──────┐
       │  Network     │                  │  Physical     │
       │  attacker    │                  │  attacker     │
       └──────────────┘                  └───────────────┘
```

Each arrow is a trust boundary.  Bytes crossing it are
untrusted from the receiving side's perspective.

## 2. Asset classes

| Asset | Trust requirement | Storage |
|-------|-------------------|---------|
| **Firmware image signing key (production)** | Confidential + integrity | OPTIGA Trust M secure NVM only |
| **Firmware image signing key (development)** | Integrity (test-only) | `keys/mcuboot_dev_ecdsa_p256.pem` -- gitignored |
| **OPTIGA Trust M device-unique key** | Confidential | OPTIGA's secure NVM (never leaves) |
| **MQTT broker TLS client cert + private key** | Confidential + integrity | Application-owned; SDK exposes the pinning API |
| **EEPROM manifest (SKU + serial + hw_rev)** | Integrity (authenticity) | 24C128 EEPROM (carrier-side); read-only at runtime |
| **Application firmware in flash** | Integrity | MCUboot-verified slot |
| **Mender update artefact** | Integrity (signed) + confidentiality (TLS in transit) | Mender server → A/B partitions |
| **OTA bootloader keys** | Integrity | MCUboot's compiled-in pub key |
| **Wi-Fi credentials (PSK / EAP)** | Confidential | Application-owned; SDK doesn't see them |
| **`board.yaml` + SoM presets (build-time)** | Integrity | Repo + downstream consumer's repo |

## 3. Adversary model

We protect against five adversary classes, ranked by capability:

### 3.1 Remote network attacker (lowest privilege)

Can send arbitrary bytes to TCP/UDP ports the device listens on.
Cannot touch the physical device or its supply chain.

**Surfaces under threat:**

- `<alp/iot.h>` MQTT (TLS handshake + frame parser).
- `<alp/ble.h>` BLE advertising / connection parser.
- Mender OTA-fetch HTTPS client.

**Mitigations:**

- MQTT runs over TLS 1.2/1.3 with pinned CA path.  Default
  bundle: `/etc/ssl/certs`.  Insecure flag exists but is
  documented as test-only.
- BLE adv parser fuzzed under `tests/fuzz/ble_adv_parser_fuzz.c`
  against the length-overrun CVE class.
- Mender artefact verified against the bootloader's compiled-in
  pub key before swap-using-scratch commit.

### 3.2 BLE proximity attacker

In RF range; can advertise/scan but not pair without consent.

**Surfaces under threat:** same as 3.1's BLE row.

**Mitigations:**

- BLE adv parser fuzzed (above).
- Pairing requires user-confirmed bonding; auto-pair disabled
  by default in the SDK's BLE backend.

### 3.3 Local I²C / I²S / UART attacker

Has access to the carrier's expansion header.  Can drive bytes
into the on-module BRD_I²C bus (PMICs, RTC, secure element,
supervisor MCU) or pretend to be a slave.

**Surfaces under threat:**

- OPTIGA Trust M APDU parser (`chips/optiga_trust_m/`).
- DA9292 / ACT88760 PMIC register handling.
- GD32G553 bridge frame parser (host side).
- 24C128 EEPROM manifest (a bad-actor master could rewrite the
  manifest in-band).

**Mitigations:**

- OPTIGA APDU parser fuzzed under
  `tests/fuzz/optiga_apdu_fuzz.c`.
- GD32 bridge framing fuzzed under
  `tests/fuzz/gd32_bridge_frame_fuzz.c`.
- EEPROM manifest carries a CRC32; runtime
  `<alp/hw_info.h>::alp_hw_info_read` rejects mismatched CRC.
  Production-side: 24C128 has a write-protect strap; carrier
  designs should tie it to "WP active" at runtime.

### 3.4 Local physical attacker (JTAG/SWD)

Direct probe access to the carrier's debug header.  Can halt the
CPU, dump RAM, reflash.

**Surfaces under threat:**

- Application code + signing keys in RAM.
- Bootloader key material if not anchored in OPTIGA.

**Mitigations:**

- Production-side: lock the SWD interface via the SoC's
  debug-disable fuse + the secure-boot lock register.  See
  the per-SoM bring-up docs (e.g.
  [`bring-up-v2n.md`](bring-up-v2n.md) "Production lock-down").
- Pre-production / dev builds: SWD open.  Customers should
  blow the lock fuse for production runs.

### 3.5 Supply-chain attacker

Has access to the production pipeline.  Can swap binaries, drop
keys, or insert a backdoored vendor library.

**Surfaces under threat:**

- Build artefacts shipped to customers.
- `west.yml` pinned dependencies.
- `keys/` development keys (production never lives in repo).

**Mitigations:**

- Release tarballs SHA-256/512 + SHA-512 checksums per the
  `release.yml` workflow.
- v1.0+ adds SLSA L2 provenance attestations (planned per
  Pillar 8 of `docs/v1.0-readiness.md`).
- `keys/.gitignore` excludes every `*.pem` file; only the
  generator script + README live in the keys dir.
- Production signing key never leaves the OPTIGA secure NVM;
  the SDK ships only the pub-key bytes compiled into MCUboot.

## 4. Per-surface threat catalogue

Each public `<alp/X.h>` header gets a row noting the threats it
faces + the mitigations in place.  Header that has nothing to
defend (e.g. `<alp/e1m_pinout.h>` is just constants) marked n/a.

| Header | Primary threat | Mitigation |
|--------|----------------|------------|
| `<alp/peripheral.h>` (I²C/SPI/GPIO/UART) | Bus-side attacker on a shared bus | Backend validates handle on every call; NULL-cfg / OOB IDs rejected at `*_open` |
| `<alp/pwm.h>` / `<alp/adc.h>` / `<alp/dac.h>` | Local physical (probe analog rails) | Out of SDK scope -- hardware-level concern |
| `<alp/iot.h>` (MQTT) | Network 3.1 | TLS default, fuzzer on URI parser, broker-reply fuzz pending |
| `<alp/ble.h>` | RF proximity 3.2 | Adv-parser fuzz |
| `<alp/security.h>` | All five | Wraps MbedTLS PSA Crypto + OPTIGA TM; secret material wiped via `OPENSSL_cleanse` on Yocto, `psa_destroy_key` on Zephyr |
| `<alp/inference.h>` | Supply-chain (Vela model swap) | Model-bytes integrity is application-owned; SDK signs the firmware containing it via MCUboot |
| `<alp/mproc.h>` (IPC framing) | Cross-core peer-compromise | Envelope decoder fuzzed under `tests/fuzz/mproc_frame_fuzz.c` |
| `<alp/hw_info.h>` | Local I²C 3.3 (manifest rewrite) | CRC32 verification on read; production WP strap on the EEPROM |
| `<alp/dsp.h>` | Application-side input fuzz | DSP-chain descriptors validated at `_open`; no untrusted-network-driven path today |
| `<alp/gpu2d.h>` | Application-side surface descriptor injection | Surface dimensions bound-checked at `_open` |
| `<alp/power.h>` | DoS via aggressive sleep request | `_open`-time validation of wake-source bitmaps |
| `<alp/storage.h>` (inline-AES) | Local physical | Key material via OPTIGA path only; SDK never sees the AES key in clear |
| `<alp/camera.h>` | DMA-buffer overflow if frame_size mis-declared | Backend reconciles frame_size against silicon-reported width × height × bpp |
| `<alp/audio.h>` | DMA-buffer overflow on misbehaving DMIC | Backend enforces `frames_per_block` × `channels` × `sizeof(int16_t)` against allocator |

## 5. Out-of-scope (explicit non-goals)

- **Side-channel attacks** (DPA, EMA, timing).  Out of scope
  for v1.0; the OPTIGA Trust M is the trust anchor for
  side-channel-resistant operations.  ALP SDK does not
  implement constant-time crypto -- it dispatches to MbedTLS
  PSA / OPTIGA.
- **Rowhammer-style fault attacks** -- silicon-level; not our
  layer.
- **Trust-of-vendor-SDK-source-code.**  We assume the Alif /
  Renesas / NXP / TI SDKs we link are not actively malicious.
  Upstream CVEs trigger our pin bumps; we don't audit their
  source per-release.

## 6. Review cadence

- **Pre-tag review**: every major release (v1.0, v2.0, ...) gets
  a fresh pass over §1..§4; significant new headers added to
  the catalogue + the review captures any new threat surface
  the release introduced.
- **External audit**: v1.0 prep includes one external
  security audit by a third-party firm (TBD; tracked in
  Pillar 8 of `docs/v1.0-readiness.md`).  Findings get patched
  + retro'd against the catalogue here.
- **Per-CVE response**: covered by
  [`docs/security-advisories.md`](security-advisories.md).

## See also

- [`docs/secure-boot.md`](secure-boot.md) -- trust model
  detail + MCUboot key lifecycle.
- [`docs/security-advisories.md`](security-advisories.md) --
  embargoed-disclosure workflow.
- [`docs/abi-markers.md`](abi-markers.md) -- which surfaces are
  frozen vs experimental (affects the audit's stability
  assumptions).
