### Added — RZ/V2N OTP recorded in the SoC metadata and the V2N SoM doc (#1148)

`metadata/socs/renesas/rzv2n/n44.json` now counts the SoC's OTP unit
(`"otp": 1`). `docs/soms/v2n.md` gains a "SoC OTP" section with the facts
from the hardware manual (R01UH1071EJ0120 Rev.1.20 §4.10):
- 32 Kbits, supplied from `OTPVDD18`;
- base `0x10450000` (CM33 `0x50450000` / `0x40450000`);
- 16-bit write, 32-bit read, write-once bits;
- the Table 4.10-3 area map: chip product ID `0F3h`-`0F6h`, one-time-read
  enable `12Ah`, boot-device drive strength `12Ch`, user areas `160h`-`1DFh`
  and `1E0h`-`3DFh`.

The SDK does not use it (identity is EEPROM-authoritative). The doc records
that writing it is permanent and out of scope for provisioning.
