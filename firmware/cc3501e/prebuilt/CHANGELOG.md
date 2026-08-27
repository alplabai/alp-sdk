# CC3501E prebuilt firmware — release notes

Each entry corresponds to a tagged release of the in-tree
[`firmware/cc3501e/`](..) source.  The signed binary, its detached
signature, and a SHA-256 manifest are dropped into this directory and
named `cc3501e-vX.Y.Z.bin` (matching `firmware/cc3501e/firmware-version.txt`).

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.4.0]

Built with the `ti` backend (TI `ticlang` 5.1.1 + SimpleLink Wi-Fi SDK
10.10.01.08 + SysConfig 1.28.0 + Wi-Fi toolbox 4.2.4) via
`firmware/cc3501e/ti/build_ti.ps1 -Ble`.

- `cc3501e-v0.4.0.bin`         -- signed firmware image (full shipped stack)
- `cc3501e-v0.4.0.bin.sig`     -- detached **ECDSA-P256/SHA-256** signature
  (the VALIDATION vendor key -- a bench-grade artifact, not production-key;
  verify with `openssl dgst -sha256 -verify <pub> -signature
  cc3501e-v0.4.0.bin.sig cc3501e-v0.4.0.bin`)
- `cc3501e-v0.4.0.bin.sha256`  -- integrity manifest (`a73e0555841e90522eaa4c007445a4f08331092842267be200fd0c895ed4881a`)

**THREE version numbers, and they are not interchangeable.** Conflating them
has cost bench time repeatedly, so this release records all three:

| Number | Value | What it gates |
|---|---|---|
| App SemVer (`firmware-version.txt`) | **0.4.0** | the `fw_version` marker the bridge reports in `DIAG` (`0x0400`) |
| Wire protocol (`ALP_CC3501E_PROTOCOL_VERSION`) | **5** | `GET_VERSION`; a host on another version is refused outright |
| GPE image `--version` (this artifact's stamp) | **0.4.0.0** | the vendor-RoT **anti-rollback** floor burned into the part |

**Wire protocol 4 -> 5.** Adds `OTA_UPDATE_MODE` (opcode `0x47`): asks the
device to reboot into (or out of) update mode. A host built from this tree
expects **5** and will refuse the previous `cc3501e-v0.3.0.bin`, which answers
**4** -- upgrading host and companion is not optional across this boundary.

Also in this release, relative to 0.3.0:

- OTA over the bridge: 1373 s -> 8 s, and 16 defects fixed including a silent
  image splice (#1610, #1655).
- `PROMOTE` is now the sole OTA commit, confirmed from flash (#1123, #1714).
- Async events fan out to every subscriber instead of a single callback that
  one consumer could steal (#1724), and get an attention edge on the READY
  wire (#130, #1721).
- `WIFI_AP_START` has a real success path (#1696, #1709); a failed connect no
  longer leaves a stale association, and RSSI is validated (#1703).
- The Wi-Fi radio is driven from the power presets, on the task, with the
  result actually reported (#1681).
- The TI build no longer ships a stale image after a failed build (#1722,
  #1726) -- this one silently invalidated earlier bench results, because a
  build that died mid-compile left the *previous* `.out` flashable.

**Anti-rollback -- read this before flashing:**

- This artifact is stamped **`0.4.0.0`**, derived from the app SemVer with
  `major = 0`. A GPE major `>= 1` fails BL2 secure-boot with `AUTH_ERROR`.
- The stamp must be **monotonically >=** anything ever flashed on that part.
  A part whose floor is already higher (any unit used for OTA testing) will
  **refuse this artifact**: re-wrap the same `.out` at a legal stamp rather
  than assuming the binary is bad. Only the stamp differs.
- **The `--version` you build with must match the version stamped into the
  signed `programming_instructions`** in the flash-set, or the programmer
  reports success and the device silently keeps the old image.
- A rollback attempt streams clean and then refuses to boot. That reads as a
  dead part; it is not -- it is the anti-rollback gate doing its job.

**Do not derive a stamp from `regen_flashset.sh`'s epoch scheme for a
release.** `0.$(((e>>16)&255)).$(((e>>8)&255)).$((e&255))` wraps its high byte
every ~194 days, so it is *not* monotonic over a part's life: run today it
yields `0.144.51.196`, which is **below** a floor of `0.149.63.0` already
burned into this bench's unit. It is fine for same-day bench iteration, which
is all it claims; it is not a release scheme.

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
