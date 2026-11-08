### Fixed — examples: aen-ospi-regcheck readiness gate and restore path, bound MDIO pre-issue wait (#2993)

aen-ospi-regcheck now reports RESULT FAIL when the OSPI device is not ready (device_is_ready), and its program/erase self-test restores sector 0x01FF0000 on every failure path after the erase instead of returning early. The MDIO pre-issue busy wait in aen-power-domains and aen-ethernet-link is now bounded, so a GMAC without its clock no longer hangs the example before it prints any result. Refs #2993.
