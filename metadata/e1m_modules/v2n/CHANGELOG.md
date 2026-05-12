# E1M-X V2N hardware-revision change log

Per-rev summary of pin / component / configuration changes on the
V2N SoM family.  Customer-facing -- describes what changes between
revs so firmware engineers can keep compatibility across the
`min_sdk_version` / `max_sdk_version` windows declared in
[`hw-revisions.yaml`](hw-revisions.yaml).

Detailed schematic-level rev notes (resistor IDs, layout deltas,
design rationale) live in the private companion repo.

## r1 -- production (initial release, 2026-05-12)

* First production revision.
* Renesas RZ/V2N + GD32G553 supervisor MCU + Murata
  LBEE5HY2FY-922 Wi-Fi/BT + 2 × RTL8211FDI Ethernet PHYs +
  ACT88760 primary PMIC + DA9292 secondary PMIC + Renesas /
  IDT 5L35023B clock generator + RV-3028-C7 RTC + OPTIGA Trust M
  secure element + TMP112 temperature sensor + N24S128 EEPROM.
* `min_sdk_version: 0.3.0` (first SDK that ships V2N drivers).
* HW-design decisions captured 2026-05-12:
  * Renesas-side `E1M PWM6` / `PWM7` reassigned to `DA9292_TW`
    (P36) / `DA9292_INT` (P37); PWM6 + PWM7 are now GD32-only.
  * `GD32_NRST` → Renesas `P74`; open-drain (shared with primary
    PMIC reset-out); was `E1M PWM4 / GPT4_GTIOC4A`.  PWM4 stays
    Renesas-driven if the host doesn't actively drive NRST.
  * `GD32_SWDIO` + `GD32_SWCLK` planned to route from V2N to two
    TBD Renesas pads (SWD bit-bang from the host enables
    universal GD32 reflash without needing factory-ISP /
    BOOT0-strap).
  * BOOT0 reassignment dropped (earlier plan to put BOOT0 on
    P75 superseded by the SWD-from-host design).

## r2..r8 -- reserved

Future revs land here as the SoM respins.  Each rev's section
documents:

* What changed since the previous rev (pin / chip / passive deltas).
* Why it changed (link to design-rationale memory or internal
  doc).
* Compatibility impact on existing firmware (`min_sdk_version`
  bump? Kconfig flag flip? new chip driver opt-in needed?).
* Migration notes for customers upgrading firmware between revs.

## How to read this with `hw-revisions.yaml`

The `min_sdk_version` / `max_sdk_version` window per rev in
[`hw-revisions.yaml`](hw-revisions.yaml) is the **enforced**
compatibility contract: the SDK loader refuses to build firmware
whose SDK version falls outside that window for the declared
`som.hw_rev`.

This file is the **human-readable explanation** of what each rev
contains -- use it to decide whether a code change you're making
needs to gate on a specific rev.
