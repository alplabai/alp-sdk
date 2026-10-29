### Fixed — metadata: mark AEN OPTIGA DNP, route GD32-owned V2N pads, tighten preset I2C address schema, join SoM and carrier I2C buses (#2958, #2959, #2990)

E1M-AEN301/401/501/601/701 listed optiga_trust_m with no assembled key, so
it counted as fitted although every AEN SKU shares one SoM PCB on which IC1
is DNP (AEN801/803 already said so). All AEN SKUs now carry assembled:
false and capabilities.optiga_trust_m: false, so the capability follows
population and an attestation root on OPTIGA is refused on every AEN SKU.

V2N/V2M pad_routes omitted the GD32-owned E1M_X_I2C3 (PC8/PC9) and
E1M_CAN_STBY (PB13) pads, which defaulted to direct. Both are now routed
through gd32_bridge in all six V2N/V2M presets.

V2N hw-revisions r2 gets min_sdk_version 0.17.0 and a changes record, and
v2n/CHANGELOG.md no longer lists r2 as reserved.

som-preset-v2: i2c_device.address_7bit is required and its pattern is
limited to 0x00-0x7F.

check_i2c_address_uniqueness now joins a carrier's i2c_devices with the
SoM e1m_i2c bus of every hosted family, so a carrier part on the module
EEPROM address fails.
