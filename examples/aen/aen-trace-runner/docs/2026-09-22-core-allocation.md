# Trace Runner — core allocation across the E8

**Status:** proposal. Nothing here has run on hardware.

**Source for every number below:** `metadata/socs/alif/ensemble/e8.json` in
alp-sdk. Where a claim comes from a bench run instead, the run is named.

## What the part actually has

| Unit | Count | Clock | Vector | Local memory |
|---|---|---|---|---|
| Cortex-A32 | **2** | 800 MHz | NEON | 32 KB L1 each, 512 KB **shared** L2 |
| Cortex-M55 HP | 1 | 400 MHz | Helium | 1280 KB TCM |
| Cortex-M55 HE | 1 | 160 MHz | Helium | 512 KB TCM |
| Ethos-U85 | 1 | 400 MHz | — | 204 GOPS |
| Ethos-U55 (HP-paired) | 1 | 400 MHz | — | 204 GOPS |
| Ethos-U55 (HE-paired) | 1 | 160 MHz | — | 46 GOPS |

On-chip RAM: **9,984 KB** total. MRAM: 5.5 MB at `0x80000000`.

Six programmable units. **The game currently uses one of them — the 160 MHz
M55-HE, the slowest.** That is the single most important fact in this document.
Every performance concern raised so far was measured against the weakest core
on the die.

## The constraint that actually binds

Not compute. **Framebuffer bandwidth.**

- Panel: 720x1280 RGB565 = **1,843,200 B per frame**.
- CDC200 scans out continuously at the 40 MHz pixel clock: roughly **80 MB/s of
  SRAM reads**, always, whatever the CPUs are doing.
- A full-screen repaint at 30 fps adds roughly **55 MB/s of writes**.
- Double-buffering costs 3.7 MB of the 9.75 MB on-chip total — 38% of all SRAM
  for two framebuffers.

This is why HyperRAM matters far more than core count: 64 MB of external memory
turns the framebuffer question from "does it fit" into "which core draws it".
See the HyperRAM section below.

## Proposed allocation

### M55-HE (160 MHz) — real-time I/O

Keeps what it already has and already works: the panel (CDC200/DSI/D-PHY), the
camera (CSI-2/CPI), the IMU, and the console. All of it is DMA-driven, so the
core's duty cycle is low and 160 MHz is not the limit — the detector runs on a
64x40 decimated grid, which is a few thousand integer operations per frame.

Also the boot role: per the bench-proven recipe, the SES boots the **HP** entry
and HP releases HE at runtime. HE is not the master.

### M55-HP (400 MHz) + Ethos-U85 — game logic and inference

2.5x the HE's clock, 1280 KB of TCM, and the 204 GOPS NPU paired to it.

This is where a real person-detector belongs. The current classical
background-subtraction tracker exists because `person_detect` is a classifier
with no bounding box and nothing better was reachable. With 204 GOPS available,
a genuine detection model becomes viable, which restores the NPU story the
demo currently lacks — and it can run *beside* the classical tracker rather
than replacing it, so a model failure degrades instead of breaking.

### A32 x2 (800 MHz each) — the renderer

The perspective renderer is embarrassingly parallel by scanline: on a flat
ground plane every scanline is one constant depth, so it is a horizontally
scaled copy of a single texture row. Split the frame in half, one A32 per half.
The 512 KB **shared** L2 is a real advantage here — both cores read the same
texture and sprite data, so it stays hot for both.

The A32s touch no peripheral. They read a display list and write a back buffer.
That is what makes this tractable: no DRM, no V4L2, no display bring-up on a
second core.

## Sequencing — do not build this all at once

Staging matters more than the destination. Every stage below is independently
useful and independently verifiable, and each one answers whether the next is
needed.

### Stage 0 — fill-rate probe (no hardware dependency to write)

A small app that repaints full-screen at increasing rates and reports achieved
fps and where it saturates. Build it for **both** `rtss_he` and `rtss_hp` so
one bench session produces two numbers.

This is the measurement the whole plan turns on. Without it, every choice below
is a guess.

### Stage 1 — move the game HE to HP

A board-target change, not an architecture change. 2.5x the clock for nearly no
work. Dual-core boot is bench-proven, so the mechanism exists.

**This may be sufficient on its own.** Do not skip past it to something
grander before measuring.

### Stage 2 — HP renders, HE does I/O

Split across the two M55s over the **proven** MHU-1 doorbell (PR #203,
`examples/aen/aen-dualcore-doorbell`, bench-verified 1:1 propagation).

Boot recipe, bench-proven (PR #201): dual ATOC with HP-APP `["load","boot"]` at
`0x50000000` and HE-APP `["load"]`-only at `0x58000000`; the SES-booted HP calls
`se_service_boot_cpu(EXTSYS_1 = 3, 0x58000000)` to release HE. HE as master
boots **neither** core.

### Stage 3 — A32 pair as the renderer

The largest piece, and the one with the most unknowns:

- **No A32 board target exists in alp-sdk.** The bare-metal backend is stubs
  plus vendor-HAL wrappers. This is a new port.
- **A32 to M55 mailbox is a different MHU instance** (`apss-mhu`) from the
  HE/HP pair, and nobody has exercised it.
- **Cache coherency is the hazard that will cost a bench session.** Two
  architectures, two cache hierarchies, one shared framebuffer. Get the
  clean/invalidate or the memory attributes wrong and the result is tearing and
  stale pixels that read exactly like a renderer bug — the failure looks like
  bad graphics code, not like a memory-attribute mistake, so it is expensive to
  diagnose. Decide the buffer's attributes deliberately and write that decision
  down before any rendering code exists.

### Stage 4 — NPU detector on the U85

Replaces or augments the classical tracker. Independent of the rendering work
and can proceed on its own track.

## Parallel track — the HyperRAM R3 board fix

Not firmware. A schematic error at **U9**: the authoritative netlist
(`E1M-AEN-2626-R2_pinmap.csv`) has `OSPI0_SCLK_R_P` on pin B1 (`PS_SCK#`) and
`OSPI0_SCLK_R_N` on B2 (`SCK`) — the differential clock pair swapped. U10, the
NOR flash beside it, is wired correctly.

The silicon is fine. Driving the clock inverted in software made the part answer
correctly on the bench: **ID0 `0x0F86`, ID1 `0x0001`**, address-dependent and
matching the datasheet.

Firmware cannot work around it: `SCPOL` does not reach the pads on AE822, and
the GPIO bit-bang that proved the diagnosis is orders of magnitude too slow to
back a framebuffer.

Fixing it yields **64 MB** of external memory, which changes the framebuffer
arithmetic completely and is what makes the A32s genuinely worth using — for
this game and for everything else.

## What is decided and what is not

**Decided, on bench evidence:** both M55 cores boot from one power-on with HP as
master; the HE/HP doorbell propagates 1:1 over the non-secure MHU-1 pair; the
secure SESS MHU pairs are not required; HyperRAM's fault is a board error at U9.

**Not decided, and not decidable without the board:** whether one core is enough
for a full-screen perspective renderer at 30 fps; what SRAM sustains under
simultaneous CDC200 scanout and CPU writes; whether the A32/M55 mailbox works.

Stage 0 exists to convert the first two from arguments into numbers.
