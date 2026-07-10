# CC3501E prebuilt firmware — release notes

Each entry corresponds to a tagged release of the in-tree
[`firmware/cc3501e/`](..) source.  The signed binary, its detached
signature, and a SHA-256 manifest are dropped into this directory and
named `cc3501e-vX.Y.Z.bin` (matching `firmware/cc3501e/firmware-version.txt`).

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

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
`firmware_path` at this blob and `flash_method` at `cc3501e_usb_bootloader`;
`flash_args.device`/`mode` stay TBD until the public cc3501e-flasher CLI
ships (they are bench/host-specific, not SoM properties).

**Full OTA cycle validated on hardware (2026-07-10):** stream → FINISH →
STAGED → the CC35's own `psa_fwu_request_reboot()` swap (the bridge drops,
then returns) → the swapped image runs and **self-accepts across a true
cold POR** (no rollback). Proven on the E1M-AEN801 EVK with a FORWARD
candidate — the OTA payload's signed version must EXCEED the running
primary (monotonic anti-rollback: a downgrade is refused at `psa_fwu`
install). A first OTA after a failed one recovers cleanly (no bridge
wedge, no CC35 reset). See `firmware/cc3501e/BRINGUP_STATUS.md` §5.
