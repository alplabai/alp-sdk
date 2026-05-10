# CC3501E firmware — bootstrap from this repo

The CC3501E Wi-Fi 6 + BLE 5.4 coprocessor on the E1M-AEN module runs
its own firmware, built and released from a separate repository
**`alplabai/cc3501e-firmware`** (per [ADR
0005](../../docs/adr/0005-alp-sdk-vs-alp-studio-boundary.md)'s
dual-use acid test — it ships TI SimpleLink CC33xx SDK, licenses
differently, releases on its own cadence, and most alp-sdk
consumers never rebuild it).

This directory is the **alp-sdk side of that boundary** — what you
need from this repo when working on the firmware or when shipping
a binary alongside an alp-sdk release.

## What lives here

| Path                              | Purpose                                                                  |
|-----------------------------------|--------------------------------------------------------------------------|
| `README.md` (this file)           | Bootstrap + contract overview.                                           |
| `prebuilt/`                       | Signed firmware binaries shipped with each alp-sdk release.  Versioned: `cc3501e-vX.Y.Z.bin` + matching `.sig` + `.sha256`. |
| `prebuilt/CHANGELOG.md`           | Per-release notes for the prebuilt binaries.  Cross-references the firmware repo's release tag. |
| `flash.py`                        | Vendor-neutral flashing helper.  Talks to the CC3501E via the Alif over SPI1 (using `chips/cc3501e/` from this repo) and writes the binary to the CC3501E's internal flash. |
| `protocol-version.txt`            | The wire-protocol version this alp-sdk release expects.  Must match `ALP_CC3501E_PROTOCOL_VERSION` in `<alp/protocol/cc3501e.h>`. |

## Source-of-truth contract

The firmware MUST implement these contracts; the Alif-side code in
this repo depends on them.  Each item points at the authoritative
definition.

| Contract                                  | Defined in                                                                          |
|-------------------------------------------|-------------------------------------------------------------------------------------|
| Wire protocol (commands, frames, events)  | [`include/alp/protocol/cc3501e.h`](../../include/alp/protocol/cc3501e.h)            |
| Bridge architecture overview              | [`docs/cc3501e-bridge.md`](../../docs/cc3501e-bridge.md)                            |
| GPIO behaviour (open-drain, edge IRQs, safe defaults) | [`docs/cc3501e-bridge.md` § "Firmware-side GPIO behaviour contract"](../../docs/cc3501e-bridge.md#firmware-side-gpio-behaviour-contract) |
| Inter-chip wiring (SPI1 + control lines)  | [`metadata/e1m_modules/aen/inter-chip.tsv`](../../metadata/e1m_modules/aen/inter-chip.tsv) |
| E1M ↔ CC3501E pad routing                 | [`metadata/e1m_modules/aen/from-cc3501e.tsv`](../../metadata/e1m_modules/aen/from-cc3501e.tsv) |

The firmware repo should mirror `include/alp/protocol/cc3501e.h`
into its own `include/alp/` so both sides build against the same
header.  When the protocol changes here, bump
`ALP_CC3501E_PROTOCOL_VERSION` and tag a corresponding firmware
release.

## Bootstrap checklist (when `alplabai/cc3501e-firmware` is created)

1. **Empty repo + license**: Apache-2.0 (matches alp-sdk).
2. **Vendor SimpleLink CC33xx SDK** as a git submodule under
   `vendor/simplelink-cc33xx/`.  Pin to a specific TI release tag.
3. **Toolchain**: `ticlang` (TI's LLVM fork) preferred over CCS
   Studio — works in CI without GUI.  Pin version in `.ti-cgt`.
4. **Wire protocol mirror**: copy
   `include/alp/protocol/cc3501e.h` from alp-sdk into the
   firmware repo's `include/alp/`.  Add a CI check that diffs
   against alp-sdk's copy at the pinned tag.
5. **SPI-slave parser**: implement against the protocol header.
   Parse `<header (4B), payload (≤512B)>` frames; dispatch
   per-command.
6. **Wi-Fi**: route `WIFI_*` commands to TI's `sl_WlanXxx()` and
   `sl_NetAppXxx()` APIs.  Station, AP, and provisioning modes.
7. **BLE**: route `BLE_*` commands to TI's BLE host (Apache
   2.0–licensed; ships with the SimpleLink SDK).  GAP + GATT.
8. **GPIO proxy**: drive the proxied pads
   (`GPIO_2 / 13..30` per `from-cc3501e.tsv`) per the GPIO
   behaviour contract — open-drain for W_DISABLE1/2, edge IRQs
   for M2E_*_WAKE + BMI323_INT1, safe defaults for mux pins.
9. **Camera enables**: `GPIO_0` / `GPIO_1` drive `CAM_EN_LDO0` /
   `CAM_EN_LDO1`.  Default OFF; turn on when the Alif issues
   the appropriate `WIFI_GPIO_WRITE` (or a dedicated camera
   command if we add one).
10. **Release process**:
    - Tag `vX.Y.Z`; CI builds + signs the binary.
    - Drop the signed binary, signature, and SHA-256 into
      this directory's `prebuilt/` via a PR to alp-sdk.
    - Update `protocol-version.txt` if the protocol bumped.
    - Update `prebuilt/CHANGELOG.md`.

## Flashing (consumer-side)

End-users running an alp-sdk example don't clone the firmware
repo; they just flash the prebuilt binary.  The recommended flow:

```bash
# From an alp-sdk checkout with a programmed Alif main MCU:
python firmware/cc3501e/flash.py \
    --binary firmware/cc3501e/prebuilt/cc3501e-v0.1.0.bin \
    --verify
```

The flasher talks to the Alif over the standard debug probe
(`west flash` for Zephyr targets, OpenOCD for bare-metal), then
the Alif relays the firmware image to the CC3501E over the
inter-chip SPI1 using the bootloader entry sequence.  The CC3501E
verifies the signature against its OTP-burned public key before
committing the image to internal flash.

## Why two repos (not a monorepo)

Per [ADR 0005](../../docs/adr/0005-alp-sdk-vs-alp-studio-boundary.md):

- **Different licenses**: alp-sdk is Apache-2.0 clean.  The
  CC3501E firmware vendors TI's SimpleLink SDK, which carries a
  TI BSD-3 + restricted-use clause.  Keeping them separate keeps
  the alp-sdk license story clean.
- **Different release cadence**: alp-sdk lands daily; the
  firmware lands per-feature (weeks).
- **Different audience**: 99 % of alp-sdk consumers never touch
  the firmware — they grab the prebuilt binary, flash it, and
  move on.  Putting the firmware sources in alp-sdk would more
  than double the repo size for no consumer benefit.
- **Different toolchain**: ticlang + SimpleLink build is heavy
  (~3 GB) and CI-slow.  Coupling alp-sdk's CI to it would punish
  every alp-sdk PR.

## Status

| Milestone                                   | Status                                       |
|---------------------------------------------|----------------------------------------------|
| alp-sdk side: wire-protocol header          | ✅ landed (`include/alp/protocol/cc3501e.h`) |
| alp-sdk side: SPI client                    | ✅ landed (`chips/cc3501e/`)                  |
| alp-sdk side: bridge architecture doc       | ✅ landed (`docs/cc3501e-bridge.md`)          |
| alp-sdk side: firmware bootstrap doc        | ✅ landed (this file)                         |
| alp-sdk side: `flash.py` helper             | 🟡 stub — full impl when first binary lands  |
| alp-sdk side: `prebuilt/` populated         | 🔮 v0.2 — depends on firmware repo v0.1      |
| Firmware repo: `alplabai/cc3501e-firmware`  | 🔮 separate task — not yet created           |
