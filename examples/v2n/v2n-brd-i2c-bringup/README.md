# v2n-brd-i2c-bringup

Patch-day diagnostic for the V2N SoM's BRD_I2C management bus
(Renesas RIIC8). Scans the bus, distinguishes a bus-level electrical
fault (line held low / missing pull-ups / wrong pinmux) from
per-device failures, then probes every populated IC read-only and
prints a PASS/FAIL/SKIP table.

Probed devices (addresses per `metadata/e1m_modules/E1M-V2N101.yaml`):
DA9292 (0x1E), ACT88760 (0x25+0x26), OPTIGA Trust M (0x30), TMP112
(0x40 per metadata -- the example also tries the datasheet 0x48..0x4B
range and reports the discrepancy), TPS628640 (0x4D, assembly
option -> SKIP when absent), RV-3028-C7 (0x52), 5L35023B (0x68), and
the GD32G553 supervisor over its I2C bridge transport (0x70).

The example never writes a PMIC voltage, enable, or control
register. Build for the V2N M33 target via the standard alp flow;
the twister scenario is build-only on native_sim.
