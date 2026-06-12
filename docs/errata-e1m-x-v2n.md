# Errata — E1M-X-EVK carrier + V2N-M1 SoM (RZ/V2N r9a09g056n48)

Hardware findings from bench bring-up that affect the device tree /
board design and are **not** captured by the connector-derived
metadata. Confidence + the workaround in software are noted per item.
Filed for ALP review; some need confirmation that they are rev-wide
(vs. unit-specific) against the authoritative schematic.

## E1: Ethernet MDI pair-order reversal (LAYOUT BUG — needs respin)

**Symptom:** No link on end0/end1 with a standard cable (NO-CARRIER);
`ANLPAR=0`, RX never locks, link LEDs dark.

**Root cause:** the PHY→RJ45 differential pairs are wired
**mirror-reversed** on the PCB: PHY MDI_A↔RJ45 pos D, MDI_B↔C, MDI_C↔B,
MDI_D↔A. The autonegotiation pair (MDI_A = RJ45 pins 1-2) physically
lands on pins 7-8, so neither side hears the other. Auto-MDIX cannot
correct a full D-C-B-A mirror (it only does A↔B / C↔D).

**Proven:** splicing a pair-mirror patch cable (swap RJ45 positions
(1-2)↔(7-8) and (3-6)↔(4-5)) → end0 `Link is Up, 100Mbps/Full`,
carrier=1. (100M, not gigabit, only because the hand-spliced cable
can't meet 1000BASE-T SI; proves the diagnosis regardless.)

**Fix:** correct the PHY→magnetics→RJ45 pair mapping in the layout
(respin) → native gigabit, any standard cable. end1 almost certainly
has the identical reversal.

**Software workaround:** none possible (physical wiring).

**Confidence:** high (bench-proven). Confirm rev-wide.

## E2: PHY latches MDIO address 0 (alias 2), not the strapped 4

**Symptom (if DT uses reg=0):** `RTL8211F … stmmac-N:00 phy_poll_reset
failed: -110`, "Cannot attach to PHY".

**Root cause:** the RTL8211F-VD PHYAD strap pins are shared with the MAC
RGMII RXD3/RXC/RXCTL lines. The MAC's push-pull drivers override the
weak (~4.7k) strap pulls during the reset-latch window, so the PHY
samples MAC-driven levels and latches **address 0** (with a mirror
alias at 2) instead of the schematic-strapped 4. Address 0 is the
broadcast-ish alias where phy_poll_reset misbehaves; address **2** is
the clean unicast alias where the PHY attaches as `stmmac-N:02`.

**Software workaround (shipped):** device tree uses `reg = <2>` on both
phy nodes (DT patch 0013). Both PHYs then attach cleanly.

**Optional HW fix (deterministic address):** isolate the PHYAD straps
from the MAC RGMII lines with series resistors, or hold the MAC RGMII
tristated until after PHYRSTB deasserts.

**Confidence:** high (MDIO scan + driver attach, both ports, multiple
sessions).

## E3: USB2.0 over-current pins read permanently asserted

**Symptom:** every boot logs `usb usb2-port1: over-current condition`
and `usb usb3-port1: over-current condition` (the EHCI/OHCI USB2.0
companion channel).

**Root cause:** the SoC OVC pins (P9.6 for the usb20 channel, PB.1 for
usb30) read asserted on this carrier — the OC sense is not usable as
wired. Removing OVC from the pinctrl groups alone is insufficient
(U-Boot leaves the pins in OVC function), and stock OHCI has no
software over-current-ignore knob (addressed by kernel patch 0003 in
the second revision below).

**Software workaround (shipped, since revised):** DT patch 0012 —
`/delete-node/ ovc` from usb20_pins/usb30_pins **and** a gpio-hog
claiming P9.6 + PB.1 as GPIO inputs (deselects the OVC peripheral
function → internal OVRCUR reads inactive). EHCI/OHCI stay enabled;
USB2.0 host fully functional. *That verification predates the GD32
SCI7 supervisor link.*

**Revision (2026-06-12):** the P9.6 half of the hog REGRESSED when the
CM33-owned GD32 supervisor SPI took P96/SCK7 (2026-06-03). The CM33 is
started by BL2 **pre-Linux** (see `rzv2n-m33-secure-boot.md`), so its
SCK7 mux is live before Linux boots; the hog's PMC9 byte-RMW at ~1.9 s
then *clobbered the running link's clock pad* at every Linux boot until
the CM33's own pin re-init took the pad back — the same clobber class
as SD1_CD/P94, and the reason usb20 OVC suppression was lost. The hog
is now **PB.1-only**; the two usb20-channel boot lines
(`usb2-port1`/`usb3-port1`) are expected until OVC is suppressed at the
controller level — done in the second revision below (`spurious-oc` on
both controllers; no kernel-cmdline change needed).

**Controller-level suppression (2026-06-12, second revision):** the
usb20 channel's OC processing is now disabled at the controllers —
`spurious-oc` on the EHCI (generic in-tree binding) and on the OHCI
(kernel patch 0003 adds the same property, setting NOCP/clearing OCPM
in root-hub descriptor A). This removes both the boot lines and the
functional OC side-effects (hub port power-cycling on OC events).
Disabling OC processing is correct on this carrier: VBUS is hardwired
always-on with no per-port power switching to protect.
*Cold-boot-verified on the bench 2026-06-12 (patched kernel +
spurious-oc dtb): zero over-current lines from either controller.*

**Open question (bench):** USB2.0 *host enumeration* should still be
re-verified with a device plugged once the spurious-oc build is
deployed — the original "fully functional" verification predates the
P9.6 regression. Record the result here.

**Suggested metadata:** an `ovc_wired: false` flag per USB channel in
the carrier YAML would let the generator emit this automatically.

**Confidence:** high for PB.1/usb30 (no xHCI OVC in boot logs); the
usb20 regression mechanism is from the 2026-06-12 boot-log audit
(hog applies at ~1.9 s, OVC lines return at ~3.9 s, CM33 link healthy)
— controller-level suppression + host-under-OC behavior to be
HW-verified when they land.

---

### Already correct in the SDK metadata (no action — listed for closure)
- Chip BOM (TAS2563, RTL8211FDI, sensors, PMICs, GD32, etc.)
- RIIC0/1/2/8 pad routing (renesas-peripheral-map.tsv) — RIIC3/6/7 are
  not bonded out; DT patch 0011 disables them (matches the map).
- PHY identity RTL8211F-VD (`0x001c.c878`).
- TAS2563 on I2S0 with the I2S path-mux GPIOs (IO4 EN / IO5 SEL).

### Excluded as unit-specific (not errata)
- A broken ferrite bead on one unit's end0 AVDD33 rail (DMM-confirmed
  open; bench-jumpered). Handling/reflow damage on a single board, not
  a design issue.
