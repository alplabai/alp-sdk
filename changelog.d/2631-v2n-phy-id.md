### Fixed — V2N/V2M device tree forces the PHY id the silicon reports, not the BOM name (#2631)

The mdio0/mdio1 `ethernet-phy@2` nodes forced `0x001c.c878` (the RTL8211F-VD id); the fitted PHY reports `0x001c.c916` (package marking not checked), so they now force `ethernet-phy-id001c.c916` and Linux binds "RTL8211F Gigabit Ethernet". The c916 driver entry ends `config_init` with a soft reset, so ifup/resume renegotiates. See `docs/errata-e1m-x-v2n.md` E2. Built into the dtb; not run on a board.
