# v2n-ethernet-dual

Brings up both RTL8211FDI Ethernet PHYs on the V2N module (ET0 +
ET1), exercises the management surface (MDIO probe, soft-reset,
autoneg restart, link-status read), and configures Wake-on-LAN.

## What it does

For each of the two on-module PHYs:

1. `rtl8211fdi_init(...)` -- MDIO probes the chip, reads PHYID1 +
   PHYID2.  Confirms the Realtek OUI (`0x001C`).
2. `rtl8211fdi_soft_reset(...)` -- IEEE-spec BMCR reset; waits
   for the chip to self-clear within 500 ms.
3. `rtl8211fdi_restart_autoneg(...)` -- BMCR autoneg enable +
   restart.
4. `rtl8211fdi_get_link(...)` -- decodes the Realtek-extended
   page-`0xA43` reg-`0x1A` PHY-specific status to report
   up/down + speed + duplex.

Then configures Wake-on-LAN on PHY 0 with a locally-administered
MAC.

## Expected output (real V2N with both Ethernet jacks connected)

```
[v2n-ethernet] dual RTL8211FDI exercise
[phy0] init  -> status=0  PHYID1=0x001c PHYID2=0xc916
[phy0] reset -> status=0
[phy0] autoneg restart -> status=0
[phy0] link  -> status=0 up=1 speed=1000 full_duplex=1
[phy1] init  -> status=0  PHYID1=0x001c PHYID2=0xc916
[phy1] reset -> status=0
[phy1] autoneg restart -> status=0
[phy1] link  -> status=0 up=1 speed=1000 full_duplex=1
[phy0] wol_set_mac -> status=0
[phy0] wol_enable  -> status=0
[v2n-ethernet] done
```

## Expected output (native_sim)

```
[v2n-ethernet] dual RTL8211FDI exercise
[phy0] init  -> status=-2
[phy1] init  -> status=-2
[v2n-ethernet] done
```

`status=-2` (`ALP_ERR_NOT_READY`) on native_sim is expected --
the mock MDIO callbacks return zeros for every register, so the
Realtek OUI check fails.  The point of the native_sim build is
to confirm the example compiles + the API call shape is correct.

## Adapting to your board

The MDIO controller bindings vary per Zephyr board.  In the
`#ifdef CONFIG_MDIO` block of `src/main.c`, replace the placeholder
`NULL` device-tree references with your board's actual MDIO
controllers, e.g.:

```c
const struct device *mdio0 = DEVICE_DT_GET(DT_NODELABEL(mdio0));
const struct device *mdio1 = DEVICE_DT_GET(DT_NODELABEL(mdio1));
```

Adjust the PHY addresses (default 0 + 1) per your schematic's
PHYAD strap.

## See also

* [`<alp/chips/rtl8211fdi.h>`](../../../include/alp/chips/rtl8211fdi.h) -- driver API.
* [`docs/soms/v2n.md`](../../../docs/soms/v2n.md) -- V2N module reference.
* [`docs/bring-up-v2n.md`](../../../docs/bring-up-v2n.md) §5 -- bring-up walk-through.
