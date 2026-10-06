# Errata — E1M-X-EVK carrier + V2N-M1 SoM (RZ/V2N r9a09g056n48)

Hardware findings from bench bring-up that affect the device tree /
board design and are **not** captured by the connector-derived
metadata. Confidence + the workaround in software are noted per item.
Filed for Alp review; some need confirmation that they are rev-wide
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

## E2: PHY answers MDIO address 0 as well as its strapped address 2 (not a defect; documentation correction, 2026-10-03)

**Status:** not a defect (documentation correction, 2026-10-03). An
earlier revision of this entry blamed a PHY address strap latch fault
driven by the MAC; that explanation was wrong and has been removed.

**Observation:** an MDIO scan sees each PHY at address 0 and at
address 2. With the device tree on `reg = <0>` the driver logs
`RTL8211F ... stmmac-N:00 phy_poll_reset failed: -110`, "Cannot attach to
PHY".

**Explanation (RTL8211F(D)(I)-VD datasheet):** the PHY address straps
select address 2, which is what the device tree uses. Address 0 answers
because MDIO address 0 is the RTL8211F broadcast address, enabled by
default (PHY page 0xa43, register 24, bit 13). There is no strap-latch
defect and no hardware fix is needed.

**Requirement:** the device tree must use `reg = <2>` on both phy nodes
(the `mdio0`/`mdio1` blocks in `e1m-v2n-som.dtsi`); the PHYs then attach
as `stmmac-N:02`. Optionally the broadcast response can be disabled in
software (page 0xa43, register 24, bit 13 = 0), after which a scan sees
only address 2.

**PHY id (follow-up, 2026-10-02):** the DT used to force
`ethernet-phy-id001c.c878` (the RTL8211F-VD id), which made Linux bind the
"RTL8211F-VD" driver. Raw MII registers 2/3 on both ports of the bench
units read `0x001c`/`0xc916` (package marking not checked), so the forced id
was wrong. The id was taken from the BOM part name when the board DT was
created (c9e315805), not because reads at address 2 failed -- E2 only
concerns the PHY address.

The DT now forces `ethernet-phy-id001c.c916` (with the generic c22
fallback), the id the silicon reports, so driver selection does not depend on
an id read at MDIO bus registration. The driver probe still reads PHYCR1 and
PHYCR2 (page `0xa43`, registers `0x18`/`0x19`) at that moment and caches them
for the life of the device.

Behavioural delta in the 6.1 `realtek.c`: the c878 entry sets
`has_phycr2 = false`, so its `config_init` skips the PHYCR2 CLKOUT
read-modify-write and returns without a reset. The c916 entry performs the
CLKOUT read-modify-write (preserving the bit unless
`realtek,clkout-disable` is set) and ends `config_init` with
`genphy_soft_reset()`, so every ifup/resume soft-resets the PHY and
renegotiates. **Bench-gated:** confirm
`PHY [stmmac-N:02] driver [RTL8211F Gigabit Ethernet]`, a clean link on
both ports, that the renegotiation on ifup is acceptable, and, after a cold
boot, that page `0xa43` registers `0x18` and `0x19` on both PHYs show the
same ALDPS bits and CLKOUT_EN as a known-good boot.

**Confidence:** high (datasheet; MDIO scan and driver attach, both
ports, multiple sessions).

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

**Software workaround (shipped, since revised):** the first-revision
carrier DT —
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
Disabling OC processing is the workaround for the unusable OC sense
wiring; the USB 2.0 VBUS/role situation is in
[e1m-x-evk-usb-otg.md](e1m-x-evk-usb-otg.md).
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

## E4: Capacitor on the Ethernet PHY regulator output overloads the PHY's switching regulator (SoM)

**Symptom:** as soon as the PHYs leave reset, the module draws about
0.45 A at 15 V (normal: about 0.12 A at the same point), both PHYs run
hot, and their 1.0 V core rail sits near 0.3 V instead of its nominal
level. Linux then logs `Failed to reset the dma` on both Ethernet ports
and neither port works. Holding the PHYs in reset brings the current
back to normal. There is no hard short: the rail measures several
hundred ohms to ground with the board unpowered.

**Root cause:** the RTL8211F(I) generates its core supply with an
internal switching regulator. The module fits a 0.1 µF capacitor from
the PHY's regulator output pin (`REG_OUT`) to ground, ahead of the
regulator's inductor, on both PHYs. In Realtek's reference design that
capacitor is not fitted for the switching-regulator configuration; it
belongs to the LDO configuration only. On the switch node it is
hard-charged every switching cycle. The bill of materials is the same
on every module of this revision, and not every unit fails the same
way, so units that work today are exposed as well. It is a likely
contributor to the intermittent dead-PHY cold boots in #2582
(unproven).

**HW fix:** remove that capacitor on both PHYs. After the rework, the
module draws normal current, the core rail is at its nominal level and
both ports pass traffic. Next module revision: mark the part not fitted,
and keep the core-rail bulk capacitance within Realtek's limits.

**Software workaround:** none.

**Confidence:** high. Current before and after the rework was measured
on two units, and Ethernet worked after it on both (2026-10-02).

**PHY power-sequence constraints (datasheet):**

- 3.3 V rise time at least 0.5 ms; a faster rise (under 0.1 ms) can
  damage the PHY's internal regulator.
- 3.3 V off for at least 100 ms between power cycles.
- PHY reset held low for at least 10 ms, then at least 72 ms after
  release before any MDIO access.

## E5: Carrier link LED loads the PHY's LED0 configuration strap

**Symptom:** the RJ45 green LED is driven from the PHY's `LED0` pin,
which is also the `CFG_EXT` configuration strap, sampled at reset. The
module pulls it high (the external 1.8 V RGMII I/O supply
configuration).

**Root cause:** per the datasheet (LED and LDO configuration), a
pulled-high strap makes that LED output active-low, so the LED must be
wired with its anode to 3.3 V through a resistor and its cathode on the
pin. The carrier wires the green LED with its cathode through a resistor
(1 kΩ) to ground, which is correct only for a pulled-low strap. That
resistor loads the strap during the reset-latch window (risk: the PHY
reads external-supply = 0 and enables its internal LDO against the
module's 1.8 V rail) and inverts the LED. The yellow LED on `LED1` (strap pulled
low, active-high) is wired correctly.

**HW fix (next carrier revision):** reverse the LED on `LED0`, on both
ports: anode to 3.3 V through about 510 Ω to 1 kΩ, cathode to the pad.
**Interim rework (stays):** remove the `LED0` series resistor on both
ports, so the strap is not loaded; the green LED then stays dark.

**Software workaround:** none.

**Confidence:** high (datasheet and design check). The rework is applied
on the bench carrier (2026-10-02); the bench strap level was not
measured.

## E6: DRP-AI vendor driver writes CPG bits the V2N manual marks reserved (software)

**Symptom:** none observed. The DRP-AI probe and inference ran on silicon
(#1268) with the driver unmodified.

**Root cause:** the vendor DRP-AI driver we inherit (`meta-rz-drpai`
`0002-enable-drpai-driver.patch`) initialises its clocks and bus-stop bits
with a V2H-derived routine. It writes bits the RZ/V2N manual lists as
reserved: `CPG_CLKON_1` bits 8-15, `CPG_CLKON_17` bits 0-2,
`CPG_BUS_8_MSTOP` bits 10 and 12-15, `CPG_BUS_9_MSTOP` bits 0-3 and
`CPG_BUS_12_MSTOP` bits 1-8. It does not touch `CPG_BUS_12_MSTOP` bits 9/10
(the MCPU-ACPU bus), so there is no CM33 conflict.

**HW fix:** none.

**Software workaround:** none. We do not hand-patch the vendor driver; ask
Renesas whether the reserved-bit writes are safe on V2N.

**Confidence:** high for the register list (read from the patch and the
manual); safety of the writes is unconfirmed.

## E7: SWINT unit 12 raises GIC_SPI 404, which the manual lists as reserved (SoC documentation)

**Symptom:** the CM33-to-A55 reverse doorbell uses MHU SWINT unit 12, which
the manual does not document as an A55 interrupt source. The manual lists
CA55 SPI 404-411 as reserved, `swint_ch22_ns`..`swint_ch25_ns` as SPI
412-415, and `swint_ch12_ns` as not used.

**Root cause:** manual and silicon disagree. The silicon measurement
(#697 cycle 10) is that SWINT unit 12 fires CA55 INTID 436, i.e. GIC_SPI
404. The device tree and the UIO driver follow the measurement.

**Software workaround:** keep SPI 404. Do not "correct" the device tree to
the manual's table; that breaks the reverse doorbell.

**Confidence:** high on the measured behaviour, low on the vendor's
intent; Renesas has not confirmed it.

---

### Already correct in the SDK metadata (no action — listed for closure)
- Chip BOM (TAS2563, RTL8211FDI, sensors, PMICs, GD32, etc.)
- RIIC0/1/2/8 pad routing (renesas-peripheral-map.tsv) — RIIC3/6/7 are
  not bonded out (matches the map).
- PHY family RTL8211F(I); the fitted id reads `0x001c.c916` (see E2 follow-up), not the `0x001c.c878` the DT used to force.
- TAS2563 on I2S0 with the I2S path-mux GPIOs (IO4 EN / IO5 SEL).

### Excluded as unit-specific (not errata)
- A broken ferrite bead on one unit's end0 AVDD33 rail (DMM-confirmed
  open; bench-jumpered). Handling/reflow damage on a single board, not
  a design issue.
