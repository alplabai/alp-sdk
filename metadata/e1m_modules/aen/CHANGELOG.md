# E1M-AEN hardware-revision change log

Per-rev summary of pin / component / configuration changes on the
AEN SoM family.  Per the user invariant "all AENs have the same
revision, only SoC changes", the family ships one module PCB
design today; SoC-tier deltas (E3..E8) are captured in the
per-SKU `som.yaml` files, not as hw-revisions of the module.

Detailed internal rev notes are held outside this tree.

## r1 -- production (initial release)

* First production revision.  Alif Ensemble SoC (E3..E8 per the
  SKU) + TI CC3501E Wi-Fi/BT coprocessor + Infineon OPTIGA Trust M
  + Micro Crystal RV-3028-C7 RTC + TI TMP112 + Onsemi N24S128
  EEPROM + TI DP83825 Ethernet PHY.

## r2..r8 -- reserved

Per-rev format: what changed, why, compatibility impact, migration
notes.

## Per-SoC compatibility

The AEN-family `min_sdk_version` window applies module-wide;
per-SKU SoC differences are absorbed by the `silicon` field +
`<alp/soc_caps.h>` capability profile, not by the module's
hw_rev label.
