# CC3501E prebuilt firmware — release notes

Each entry corresponds to a tagged release of the in-tree
[`firmware/cc3501e/`](..) source.  The signed binary, its detached
signature, and a SHA-256 manifest are dropped into this directory and
named `cc3501e-vX.Y.Z.bin` (matching `firmware/cc3501e/firmware-version.txt`).

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.3.0]

Built on the bench with the `ti` backend (TI `ticlang` 5.1.1 +
SimpleLink Wi-Fi SDK 10.10.01.08 + SysConfig 1.28 + Wi-Fi toolbox
4.2.4) via `firmware/cc3501e/ti/build_ti.sh --wifi --ble`.

- `cc3501e-v0.3.0.bin`         -- signed firmware image (full shipped stack)
- `cc3501e-v0.3.0.bin.sig`     -- detached **ECDSA-P256/SHA-256** signature
  (the VALIDATION vendor key -- a bench-grade artifact, not production-key;
  verify with `openssl dgst -sha256 -verify <pub> -signature
  cc3501e-v0.3.0.bin.sig cc3501e-v0.3.0.bin`)
- `cc3501e-v0.3.0.bin.sha256`  -- integrity manifest

**Wire protocol 3 -> 4.** Adds `OTA_PROMOTE` (opcode `0x46`): arms the
deferred swap-reboot for an image already committed to STAGED, without
opening a new OTA session. Before it existed there was no non-destructive
way to clear *or* promote a committed STAGED image over the bridge --
`OTA_ABORT` cancels only an in-flight session, not one already committed --
so a unit left with a stuck STAGED image (e.g. a bare reset mid-swap) had no
bridge-side recovery. `OTA_PROMOTE` must be present in the CC35's *running*
firmware to help: it cannot rescue a part that is already stuck on
pre-v0.3.0 firmware, only prevent the wedge going forward.

Fixes since [0.2.0]:

- Worker-route `WIFI_AP_STOP` off the SPI ISR -- the inline call killed the
  SPI-slave DMA and nothing re-armed it (#1564).
- Stop reporting the unpopulated `WIFI_STATUS` latch byte as an RSSI
  measurement (#1387, #1420).
- Implement the wire-version refusal `DESIGN.md` always claimed -- a host on
  an incompatible protocol version is now actually rejected instead of
  silently served (#1371, #1421).
- `OTA_BEGIN` clears a stuck STAGED slot and recovers an ambiguous/failed
  slot instead of bailing, and surfaces the swap-reboot refusal rc so the
  host can distinguish "refused" from "failed" (#493).
- Dynamic BLE GATT service registration (`BLE_GATT_REGISTER`, 0x38)
  replaces the fixed-demo-service stub, plus the follow-up distinct-status
  fix for register-while-advertising (#480, #892, #895).
- The TI SPI transport left the slave permanently unarmed after a failed
  `SPI_transfer()` arm; a failure counter now drives the same full SPI
  re-open recovery the existing resync path uses (#1133).

**KNOWN DEFECT -- #1562 is OPEN:** `wifi ap` brings up a soft-AP that stops
advertising after roughly 100 seconds. Station mode, scan, MAC, BLE, sockets
and OTA are unaffected; this is soft-AP advertising only.

**Anti-rollback -- read this before flashing over a link:**

- The OTA payload's signed version must **exceed** the running primary (the
  `psa_fwu` install-time gate); a downgrade is refused, not silently
  accepted.
- Over SWD, the GPE programming version must be **monotonically >=** anything
  ever flashed on that part, with **major = 0** -- a GPE major >= 1 fails
  BL2 secure-boot `AUTH_ERROR`.
- A rollback attempt streams clean and then refuses to boot. That reads as a
  dead part; it is not -- it is the anti-rollback gate doing its job.

## [0.2.0] - 2026-07-09

First signed prebuilt, built on the bench with the `ti` backend (TI
`ticlang` 5.1.1 + SimpleLink Wi-Fi SDK 10.10.01.08 + SysConfig 1.28 +
Wi-Fi toolbox 4.2.4) via `firmware/cc3501e/ti/build_ti.sh --wifi --ble`.

- `cc3501e-v0.2.0.bin`         -- signed firmware image (full shipped stack)
- `cc3501e-v0.2.0.bin.sig`     -- detached **ECDSA-P256/SHA-256** signature
  (the VALIDATION vendor key; verify with
  `openssl dgst -sha256 -verify <pub> -signature cc3501e-v0.2.0.bin.sig
  cc3501e-v0.2.0.bin`)
- `cc3501e-v0.2.0.bin.sha256`  -- integrity manifest
  (`1dffbc30a306c5227578640d0a60b044edff1be38747ae9e57776d6b0989e9f4`)

Feature scope: the full CC3501E bridge — META (PING / GET_VERSION /
GET_MAC / RESET) + Wi-Fi station/AP + BLE (NimBLE) + sockets + OTA over
SPI. The signature is ECDSA-P256, not Ed25519 (the placeholder note in
earlier drafts was wrong — the VALIDATION vendor key is a P-256 EC key).

The AEN SoM presets' `helper_firmware.cc3501e_otp` now point
`firmware_path` at this blob; the CC3501E is never customer-flashed, so
there is no `flash_method` -- `update_channel: alp_ota_spi_otp` is the
whole story (see `metadata/e1m_modules/README.md`).

**Full OTA cycle validated on hardware (2026-07-10):** stream → FINISH →
STAGED → the CC35's own `psa_fwu_request_reboot()` swap (the bridge drops,
then returns) → the swapped image runs and **self-accepts across a true
cold POR** (no rollback). Proven on the E1M-AEN801 EVK with a FORWARD
candidate — the OTA payload's signed version must EXCEED the running
primary (monotonic anti-rollback: a downgrade is refused at `psa_fwu`
install). A first OTA after a failed one recovers cleanly (no bridge
wedge, no CC35 reset). See `firmware/cc3501e/BRINGUP_STATUS.md` §5.
