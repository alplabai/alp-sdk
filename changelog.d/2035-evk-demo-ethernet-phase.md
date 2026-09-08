### Added — `aen-evk-demo` phase 10 brings up Ethernet, and moves the image's system RAM to do it (#2035)

Phase 10 was a `SKIPPED` stub. It now powers the on-module TI DP83825 PHY,
brings up the RMII GMAC through the alp-sdk Tier-1.5 `alif,ethernet` glue, and
tests the link by pulling a DHCPv4 lease — so nine of the demo's fourteen
phases are implemented and five remain stubs. It follows
`examples/aen/aen-ethernet-link`, the bench-verified reference, in the order
that reference proved necessary.

**The PHY is powered before `main()`, and the ordering is the whole trick.**
The glue's RMII reference-clock AUTO probe runs inside the Ethernet driver's
init and chooses between the module's external 50 MHz oscillator and an
internal-PLL fallback by looking for the external clock *at that moment* — and
that oscillator sits behind the power enable. So `E_PHY_PWRDWN` (P15_4,
lpgpio) and `E_PHY_RESET` (P11_6, gpio11) are driven from a `SYS_INIT` hook at
`POST_KERNEL` priority 50, the only window that works: after the GPIO
controllers (40), before `eth_dwmac` (60). Both are **Alif** pins per
`metadata/e1m_modules/aen/alif-ethernet-phy.tsv`, not CC3501E lines. The phase
reports which source `ETH_CTRL` bit 4 latched, because "external" is the proof
the hook ran early enough.

**`PASS` is a DHCP lease and nothing weaker.** This app exists because an
earlier example counted a successful ID read as a pass, and `carrier_ok` is
that same claim wearing a better disguise: the PHY here is unmanaged (a
fixed-link devicetree child, no MDIO bus), so carrier is **synthetic** — it
reports what devicetree hard-codes and reads true with the cable in your hand.
The phase prints it labelled `SYNTHETIC, not a link proof` and never gates on
it. A lease is unforgeable by comparison, and is what caught the bug described
below: with the DMA buffers misplaced, the wire link came up and nothing moved.

**No cable is not a failure, and no DHCP server is not a failure — but a dead
data path is not a pass.** The phase reads the PHY's real status over MDIO
(raw `MAC_MDIO_*` access, because fixed-link performs no MDIO of its own),
sets `RCSR` bit 7 for 50 MHz-reference RMII, restarts auto-negotiation, and
then separates the cases with the interface's own byte counters:

* no PHY answers on any of the 32 MDIO addresses → **`FAIL`**. The DP83825 is
  fitted on every E1M-AEN SoM; silent means unpowered, unclocked or unreset —
  our hardware, not the operator's cable;
* PHY answers, auto-negotiation never completes → **`SKIPPED`**, qualifier
  `no carrier -- cable?`. The normal state of an unattended bench;
* link up but the MAC transmitted **zero bytes** → **`FAIL`**. DHCP queued
  DISCOVERs and the link is up, so no frame left the part whatever is out
  there. Unambiguous;
* link up, TX moved, no lease → **`SKIPPED`** with a qualifier that names
  which of the two worlds it is, from `rx_bytes`. Not a `FAIL`: a live switch
  port with no other talkers legitimately sends us nothing, and failing on
  that would be the mirror-image lie;
* link up **and** a lease → **`PASS`**.

**The image's system RAM moved off the M55 DTCM, and the naive version of that
move silently corrupts phase 13.** The GMAC is a DMA bus master and the
upstream DWMAC core hands it the raw CPU pointer with no translation; the DTCM
is tightly-coupled and not on the GMAC's AXI path, so rings and `net_buf`s left
there are invisible to it and zero frames move in either direction. No narrower
fix exists for an application — the descriptor rings are file-static in the
driver and the pool is file-static in the net subsystem, so neither can be
section-tagged the way phase 13's JPEG buffers are, and the driver's own header
states the requirement is enforced "at the board/SoC layer, not in this glue".

But copying `aen-ethernet-link`'s plain `zephyr,sram = &sram0` into *this* app
would have been a silent regression. The SoC's `sram0` node is both a
`zephyr,memory-region` — emitting the `SRAM0` linker region phase 13's buffers
are tagged into — and, if chosen, the source of the main RAM region. Choosing
it makes both start at the same address; the linker allocates into them
independently **and does not warn**. Measured on this tree,
`.data`/`.bss`/`.noinit` were placed on top of `jpeg_out` and `jpeg_src`, so
phase 13 writing its gradient would have scribbled over the whole image's
static state. The overlay therefore carves two disjoint windows of the same
bank: a 64 KiB `SRAM0` region at the bottom of it, holding the JPEG buffers at
exactly the addresses they had before, and a 512 KiB system-RAM window above
that, holding `.data`/`.bss`/`.noinit`, every stack, the GMAC rings and the
`net_buf` pool — the addresses themselves are in the app overlay and its
README. Shrinking `&sram0` to that window is the
safety property, not a tidy-up: an oversized `SRAM0`-tagged buffer added later
is now a **link error** instead of a silent overlap.

**What that costs the eight previously working phases is stated rather than
glossed.** `CONFIG_DCACHE` is off on this silicon, so there is no cache to
soften the move — data that used to hit single-cycle DTCM now goes to uncached
global SRAM over the fabric. Nothing becomes incorrect; things become slower.
The one phase with an inner loop tight enough to care is phase 8, whose SPI1
FIFO refill feeds a 25 MHz link and whose DW-SSI master deasserts its own
chip-select if it underruns mid-frame. That is a hypothesis, not a measurement,
and it is loud rather than silent: phase 8 prints its own verdict in the same
transcript as phase 10, so a bench run that regresses it says so, and phase 10
plus the memory map back out together if it does. `CONFIG_DCACHE=n` also
becomes load-bearing for a second reason — `eth_dwmac_alif`'s `BUILD_ASSERT`
rejects a cache-on image with no nocache region outright — so `prj.conf` now
says so where somebody might otherwise "narrow" it back.

The phase never joins anything it should not: DHCP is a lease request on the
segment the operator plugged the board into, and the app carries no static
address, no credentials and no route configuration.

Builds clean for `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`, still inside
the Flow C ITCM budget with room to spare, and with the descriptor rings, the
`net_buf` pools and phase 13's JPEG buffers verified by symbol address to land
in disjoint, DMA-reachable windows. No byte count or percentage is written into
the tree for this image — two such figures have already gone stale — so read
the linker's own summary. The phase's own `PASS` needs a bench run on
`e1m-aen-evk-03` with a cable in a live switch port; the eight previously
implemented phases are unchanged in source.
