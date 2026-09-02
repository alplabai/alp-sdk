# Threat model — Alp SDK consumer surface

A v1.0-prep threat model for the consumer-facing surface of the
Alp SDK.  Identifies trust boundaries, asset classes, primary
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
        │  Alp SDK -- public surface + loader (TRUSTED)        │
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

> **Bench status.** MCUboot's software ECDSA-P256 verification of
> slot0 is now measured working on E1M-AEN801 (`AE822FA0E5597LS0`
> Rev A0, alp-sdk `0da1f1b4`): flipping one byte of the TLV `0x22`
> signature produces `bootutil_verify_sig: ECDSA builtin key 0` then
> `Unable to find bootable image` -- the check runs to completion
> rather than hanging.  See [`docs/secure-boot.md`](secure-boot.md)
> for the full measurement.  The **customer** path -- writing slot0
> directly via J-Link, no SETOOLS/SE-UART/ATOC -- is now proven too,
> at `0x80010000` only (ATOC-region writes / erasing MCUboot itself
> untested).  Both refusal shapes (tampered signature, non-MCUboot
> image) leave the debug port alive per the §3.4 mitigation below --
> a bad slot0 write does not brick JTAG/SWD access, and this does not
> change the production recommendation there (blow the debug-disable
> fuse).  Single-slot result only -- A/B swap / OTA untested.

Each arrow is a trust boundary.  Bytes crossing it are
untrusted from the receiving side's perspective.

## 2. Asset classes

| Asset | Trust requirement | Storage |
|-------|-------------------|---------|
| **Firmware image signing key (production)** | Confidential + integrity | OPTIGA Trust M secure NVM only |
| **Firmware image signing key (development)** | Integrity (test-only) | `keys/mcuboot_dev_ecdsa_p256.pem` -- gitignored |
| **OPTIGA Trust M device-unique key** | Confidential | OPTIGA's secure NVM (never leaves) |
| **MQTT broker TLS client cert + private key** | Confidential + integrity | Application-owned; SDK exposes the pinning API |
| **EEPROM manifest (SKU + serial + hw_rev)** | Integrity (authenticity) | 24C128 EEPROM (board-side); read-only at runtime |
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

Has access to the board's expansion header.  Can drive bytes
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
  Production-side: 24C128 has a write-protect strap; board
  designs should tie it to "WP active" at runtime.

### 3.4 Local physical attacker (JTAG/SWD)

Direct probe access to the board's debug header.  Can halt the
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

- Release tarballs ship SHA-256 + SHA-512 checksums per the
  `release.yml` workflow.
- Release builds emit SLSA L3 provenance attestations per
  Pillar 8 of `docs/v1.0-readiness.md` (L2 landed in Â§C.18, upgraded
  to L3 in Â§C.27).
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
| `<alp/peripheral.h>` target (slave) mode (`alp_i2c_target_*` / `alp_spi_target_*`, v0.9) | **Untrusted external controller** drives arbitrary bytes / clock edges into our ISR-context callbacks (a hostile master is the 3.3 local-bus attacker in reverse) | The SDK layer keeps no parse surface of its own: I²C callbacks are byte-granular with no SDK-side buffering, SPI hands the app a fixed-length caller-owned frame.  Address/config validated at `*_target_open`; drivers without target support fail open with `ALP_ERR_NOSUPPORT`.  The request/response protocol on top is **application-owned untrusted input** — apps must bound-check every frame field and should fuzz their decoder (the `i2c-slave` / `spi-slave` examples model the defensive shape: fixed frames, explicit unknown-command path) |
| `<alp/version.h>` | n/a — compile-time constants + a read-only string getter; nothing to defend | — |
| `<alp/console.h>` | Companion-link peer (the bound CC3501E supervisor) | Thin binder -- stores one caller-owned `cc3501e_t*` via `alp_console_companion_set`; no parse surface of its own.  The real threats live in `<alp/chips/cc3501e.h>` (companion wire protocol) and the CLI verb, not here.  NULL unbinds safely. |
| `<alp/i2c_regfile.h>` | **Untrusted external I²C controller** drives register-pointer + payload bytes into ISR-context callbacks | Inherits the `<alp/peripheral.h>` target-mode posture it wraps: the register pointer is taken **modulo the file length** (no OOB), auto-increment wraps within the caller-owned backing buffer, and there is no SDK-side heap or parse.  Register *semantics* layered on top stay application-owned untrusted input -- apps bound-check meaning; the helper bounds memory. |
| `<alp/pwm.h>` / `<alp/adc.h>` / `<alp/dac.h>` | Local physical (probe analog rails) | Out of SDK scope -- hardware-level concern |
| `<alp/iot.h>` (MQTT) | Network 3.1 | TLS default, fuzzer on URI parser, broker-reply fuzz pending |
| `<alp/ble.h>` | RF proximity 3.2 | Adv-parser fuzz |
| `<alp/security.h>` | All five | Wraps MbedTLS PSA Crypto + OPTIGA TM; secret material wiped via `OPENSSL_cleanse` on Yocto, `psa_destroy_key` on Zephyr |
| `<alp/inference.h>` | Supply-chain (Vela model swap) | Model-bytes integrity is application-owned; SDK signs the firmware containing it via MCUboot |
| `<alp/mproc.h>` (IPC framing) | Cross-core peer-compromise | Envelope decoder fuzzed under `tests/fuzz/mproc_frame_fuzz.c` |
| `<alp/rpc.h>` (framed RPC over OpenAMP/RPMsg) | **Availability**: a compromised or crashed peer core going silent was indistinguishable from an idle one -- `alp_rpc_send()` kept returning `ALP_OK` into a dead vring forever (issue #1643), and the caller had no way to detect or react to the loss | Method-name framing is fixed-shape (≤32-byte NUL-terminated header, no SDK-side parse of the opaque application payload -- same posture as `<alp/mproc.h>`).  `alp_rpc_set_link_callback` / `alp_rpc_link_state` (v0.17) surface the transport's own bind/unbind/error signal as `ALP_RPC_LINK_DOWN`/`_UP`/`_LOST`; once `_LOST` is observed, `alp_rpc_send()` rejects with `ALP_ERR_NOT_READY` instead of silently accepting into the dead queue.  Re-establishing the link (close + reopen) stays application-owned -- the SDK reports loss, it does not auto-reconnect |
| `<alp/mproc.h>` `alp_mproc_boot_core` (v0.9) | **Privilege**: releasing a peer core with an attacker-controlled `entry_addr` is arbitrary code execution on that core | Boot-authority (SE / boot firmware) validates the request; `ALP_CORE_SELF` rejected at the API; the entry image is expected to be covered by the platform's secure-boot chain (MCUboot / SETOOLS ATOC) — the SDK call releases a core, it does not bypass image authentication.  Callers run in the firmware trust domain (no unprivileged-caller path exists on the supported RTOS targets) |
| `<alp/hw_info.h>` | Local I²C 3.3 (manifest rewrite) | CRC32 verification on read; production WP strap on the EEPROM |
| `<alp/hw_info.h>` SoC identity (`alp_soc_info_read` / `alp_soc_secure_fw_ping`, v0.9) | Spoofed / compromised secure-fw ("SE") responses over the service transport; DoS by an unresponsive controller | Bounded round-trips (`ALP_ERR_NOT_READY` instead of hangs); `out` zero-filled + build-time `soc_ref` stamped even on failure; transport faults surface `ALP_ERR_IO`.  Identity fields are **informational** — the SDK never gates an authorisation decision on them; anything security-critical stays with the secure-boot chain |
| `<alp/dsp.h>` | Application-side input fuzz | DSP-chain descriptors validated at `_open`; no untrusted-network-driven path today |
| `<alp/gpu2d.h>` | Application-side surface descriptor injection | Surface dimensions bound-checked at `_open` |
| `<alp/power.h>` | DoS via aggressive sleep request (`alp_power_request_sleep` with no reachable wake source); a caller arms a wake source (or retention footprint) the backend cannot honour, sleeps, and the device never wakes -- a silent permanent hang, worse than the DoS alone | `alp_power_request_sleep()`'s dispatcher-level pre-checks (RUN-mode / invalid-enum rejection, `ALP_ERR_INVAL` when no wake source AND `wake_after_ms == 0`) run before any backend is reached.  As of v0.17 (#1812/#1813) `alp_power_configure_wake_source()` / `alp_power_configure_retention()` additionally return `ALP_ERR_NOSUPPORT` for any bit / level the active backend cannot arm -- discoverable in advance via `alp_power_wake_capabilities()` -- instead of the old "unsupported bits silently ignored" contract, enforced centrally in the dispatcher (`src/power_dispatch.c`) against every registered backend (`zephyr_stub`, `zephyr_pm_policy`, `yocto_drv`) |
| `<alp/power.h>` operating-point profiles (`alp_power_profile_set`, v0.9) | A hostile or buggy caller browns out the core / stalls the SoC by writing a bad rail voltage or clock | Documented as a firmware-update-grade operation (header `@warning`); backend accepts only values the silicon realises **exactly** (`ALP_ERR_INVAL`, never rounds); read-modify-write touches only non-zero fields; `wake_events` writable only on the STANDBY profile; the controller firmware is the final validator |
| `<alp/storage.h>` (inline-AES, `alp_storage_configure_inline_aes`) | Local physical | Portable surface; every backend today returns `ALP_ERR_NOSUPPORT` (unimplemented) -- no backend takes the caller's key yet.  Doc contract when one lands: key/IV read only for the duration of the call, caller may free/zeroise on return, HW key never traces back to RAM after the call |
| `<alp/ext/alif/storage.h>` (Alif OSPI SecAES key provisioning, issue #224) | Local physical (JTAG/SWD) + local storage-bus attacker; **false-success risk**: an SE that never implements the write-key service must not report `ALP_OK` while the key was never loaded | Caller-supplied AES-128 key is marshalled into one on-stack SE request packet (`ospi_write_key_svc_t`) and sent to the Secure Enclave over the RTSS-HE↔SE MHUv2 mailbox (`se_service_send_request`) -- the SE, not the host, writes the key into the OSPI AES hardware slot.  That packet is the only place the SDK ever holds the key in clear; it is zeroised with a compiler barrier immediately after the call returns, on both the success and failure path, and never logged.  `se_service_send_request()` returns `rc==0` as soon as any response frame arrives, without itself checking either verdict field, so the caller reads back BOTH `header.hdr_error_code` (the transport-layer NACK an unimplemented service answers with) and `resp_error_code` (pre-seeded to a non-success sentinel before send, since a zeroed response field is bit-identical to the real success code) before ever reporting `ALP_OK`.  No OPTIGA involvement -- this is a separate key-loading path from the portable surface above.  Gated on `CONFIG_ALP_SDK_STORAGE_ALIF_SECAES`, **default OFF**: UNVERIFIED ON SILICON, no bench unit reachable at implementation time has an OSPI SecAES-relevant part populated (board rev r1; the targeted Macronix part ships DNP=0 only on the R2 BOM).  The call brackets the SE round-trip with the handle's open/close guard (issue #629) so a concurrent `alp_storage_close` cannot free/recycle the handle while the call is parked in the SE transport |
| `<alp/camera.h>` | DMA-buffer overflow if frame_size mis-declared | Backend reconciles frame_size against silicon-reported width × height × bpp |
| `<alp/audio.h>` | DMA-buffer overflow on misbehaving DMIC | Backend enforces `frames_per_block` × `channels` × `sizeof(int16_t)` against allocator |
| `<alp/update_log.h>` | Tamper of historical update records | Hash-chain + monotonic-counter (SW tier, tamper-evident); TF-M secure owner with PSA Protected Storage + protected high-watermark counter (HW_ENFORCED tier) |

## 5. Out-of-scope (explicit non-goals)

- **Side-channel attacks** (DPA, EMA, timing).  Out of scope
  for v1.0; the OPTIGA Trust M is the trust anchor for
  side-channel-resistant operations.  Alp SDK does not
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
