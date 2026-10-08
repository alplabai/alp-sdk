# fillrate probe

Stage-0 of [docs/2026-09-22-core-allocation.md](../../docs/2026-09-22-core-allocation.md): before anything gets
built on top of the E8's six compute units, this answers "can a Cortex-M55 sustain a full-screen repaint
of the 720x1280 RGB565 panel at a playable frame rate, and where does it saturate?" It is standalone --
its own `CMakeLists.txt` / `prj.conf` / `src/main.c`, no shared source with the game under `src/`.

## Build

Zephyr module is `alp-sdk-lcd`, **not** `alp-sdk` -- a plain `west build` resolves a different Zephyr
checkout that knows none of the alp-sdk shields and fails. `ZEPHYR_BASE` and `ZEPHYR_EXTRA_MODULES`
must be passed explicitly; `-p always` forces a clean configure each time.

**HE (160 MHz):**

```sh
cd examples/aen/aen-trace-runner
ZEPHYR_BASE=$ZEPHYR_BASE \
ZEPHYR_EXTRA_MODULES="<alp-sdk>;<hal_alif>" \
west build -p always -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he \
  -d build/fillrate-he probe/fillrate
```

**HP (400 MHz):**

```sh
cd examples/aen/aen-trace-runner
ZEPHYR_BASE=$ZEPHYR_BASE \
ZEPHYR_EXTRA_MODULES="<alp-sdk>;<hal_alif>" \
west build -p always -b alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp \
  -d build/fillrate-hp probe/fillrate
```

One bench session runs both -- flash HE, read the console, power-cycle, flash HP, read the console again.
Both images build to a RAM-run (`zephyr,flash = &itcm`, no MCUboot slot), exactly like the game's own
board overlay and `examples/aen/aen-dsi-display` / `aen-hp-core-smoke`.

## Run (bench)

From the `alp-sdk-lcd` checkout, using the OpenOCD RAM-run flow (the only one that can address either
M55 core on this bench, not just "whichever one the shared SW-DP happens to expose"):

```sh
cd <alp-sdk>
scripts/bench/aen/openocd-ram-run.sh examples/aen/aen-trace-runner/build/fillrate-he he
scripts/bench/aen/openocd-ram-run.sh examples/aen/aen-trace-runner/build/fillrate-hp hp
```

(`hp` is the script's default core if omitted -- pass it explicitly anyway so the command matches the
image being flashed.) Resolve the board's USB path fresh from `labgrid-client -p <place> show` per the
script's own header; do not reuse a path from a doc or a previous run. Board under test:
**a bench E1M-AEN803**. Read `ram_console_buf` over SWD after each run -- this board's console FTDI
passes zero bytes, so a UART capture will show nothing.

## Reading the output

Every run starts with the core identity and clock so two runs can never be confused:

```
=== fillrate probe: alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he ===
clock: sys_clock_hw_cycles_per_sec()=160000000 Hz  (theoretical bus peak ~640 MBps)
timing: k_cycle_get_32(), 1-cycle resolution, wraps every 26 s
icache: ON   dcache: OFF
```

The HP run should read `400000000 Hz` / `~1600 MBps` / wraps every `10 s`. **Expect HP numbers roughly
2.5x the HE numbers** (400/160 MHz) -- a result far off that ratio for the same test is a sign something
measured wrong (wrong core attached over SWD, a cache left on, a build mismatch), not a real finding.

`dcache: OFF` is the expected/intended state: `prj.conf` sets `CONFIG_DCACHE=n` because D-cache
maintenance (`SCB_EnableDCache`) hangs on this silicon (the same SoC-wide erratum every RAM-run bench app
in `alp-sdk-lcd/examples/aen` works around). If a run ever prints `dcache: ON`, that line alone means
something is wrong with the build, not with the hardware -- do not trust any Phase 1 number from that run.
D-cache's actual configured size on this part is undocumented (Cortex-M55 allows 4-64 KiB), which is
exactly why Phase 1 sweeps the working-set size below instead of assuming a size fits or doesn't.

### Phase 1 -- raw memory bandwidth, working-set sweep (no display)

One line per region x working-set size x operation, sizes in this order (4, 16, 64, 256, 1024 KiB for
SRAM0/SRAM1; DTCM stops at 64 KiB -- its whole bank is only 256 KiB on the HE core, so it physically
cannot host the larger steps), fill at every size, copy only at the smallest and largest (console-budget
tradeoff -- fill is what shows the cliff, copy at the extremes is enough to sanity-check it isn't
fill-specific):

```
SRAM0  fill     4KiB iters= 64 cycles=      1234 MBps= 512.30
SRAM0  fill    16KiB iters= 32 cycles=      4567 MBps= 501.10
SRAM0  fill    64KiB iters= 16 cycles=     18234 MBps= 460.05
SRAM0  fill   256KiB iters=  4 cycles=     75123 MBps= 447.80
SRAM0  fill  1024KiB iters=  2 cycles=    298456 MBps= 449.20
SRAM0  copy     4KiB iters= 64 cycles=      ...  MBps= ...
SRAM0  copy  1024KiB iters=  2 cycles=      ...  MBps= ...
SRAM1  fill     4KiB ...
DTCM   fill     4KiB ...
DTCM   fill    16KiB ...
DTCM   fill    64KiB ...
```

**How to read the cliff:** scan each region's `fill` column top to bottom (4 -> 1024 KiB). If the MB/s
figure is roughly flat across all five sizes (like the illustrative numbers above), the working set never
fit a meaningful cache at any tested size -- consistent with `dcache: OFF` above, and it means the 4-64 KiB
sizes are measuring the same true SRAM bandwidth as the 1024 KiB one. If instead the small sizes (4/16,
maybe 64 KiB) read noticeably *higher* than 256/1024 KiB, that gap is the D-cache -- the size where the
drop happens is the cache's actual configured size, read directly off this table instead of assumed. In
either case, **the number that matters for a framebuffer-sized workload (1,843,200 B) is the 1024 KiB
row** -- it is the closest tested size to the real thing and, if there is a cliff, it is on the far side
of it. Never quote a 4 or 16 KiB row for a framebuffer estimate even if it looks better.

`copy`'s KiB is the payload moved **one direction per pass** -- actual bus traffic for a copy is ~2x that
(one read, one write per word), stated once at the top of this section rather than on every line. A line
with `** >= theoretical peak ... suspect **` appended means the measured MB/s reached or exceeded
`clock x 4 B/cycle`, which is not physically plausible for this bus -- treat that number as broken
instrumentation, not a real result.

### Phase 2 -- renderer inner loops (no display)

```
sprite 4bpp->rgb565 96x96  iters=200 cycles=...  blits/s=...  px/s=...
scanline copy (dst=720 px fixed, nearest-neighbour Q16):
  srcW=720 (1x stretch)  iters=2000 cycles=...  rows/s=...  MBps=...
  srcW=360 (2x stretch)  ...
  srcW=180 (4x stretch)  ...
  srcW= 90 (8x stretch)  ...
  srcW= 45 (16x stretch) ...
```

These are the renderer's real inner loops (indexed sprite blit through a palette; a horizontally-scaled
scanline copy, the per-row operation a flat-ground perspective renderer performs), not synthetic
`memset()` numbers -- narrow `srcW` = a near ground row sampling a small texture strip and stretching it
hard; wide `srcW` = a far row barely stretching at all. Cost is not expected to be uniform across the
spread; that is the point of testing more than one factor.

### Phase 3 -- panel live

```
display: opened 720x1280 RGB565 (1843200 B/frame)
panel  fillOn  bytes=...  iters= 20 cycles=...  MBps=...
panel  fillOff bytes=...  iters= 20 cycles=...  MBps=...
CONTENTION: blanked=NNN MBps  live=MMM MBps  delta=DDD MBps  (scanout cost)
sustained repaint: frames=120 cycles=...  fps=NN.NN
```

`fillOn` = `display_blanking_on()` (CDC200 scanout stopped) -- isolates CPU write bandwidth to the
framebuffer with no DMA read contention. `fillOff` = `display_blanking_off()` (scanout running, ~80 MB/s
of continuous reads at the 40 MHz pixel clock) -- the contended case, and the realistic one: the panel
never stops scanning during real gameplay. **`CONTENTION`'s `delta=` line is the number the whole plan in
`docs/2026-09-22-core-allocation.md` turns on** -- how much the CDC200's continuous scanout costs the
CPU's own write bandwidth to the same SRAM.

**The arithmetic to judge `sustained repaint` against:** 720x1280 RGB565 = 1,843,200 B per frame. 30 fps
needs 30 x 1,843,200 = 55,296,000 B/s = **~55.3 MB/s** of writes, on top of the CDC200's continuous
~80 MB/s of reads -- i.e. ~135 MB/s of total SRAM traffic just to hold 30 fps, before the renderer does
any actual drawing. A good result: `fps=` at or above 30 with `live=` comfortably above 55 MBps. A bad
result: `fps=` well under 30, or a `live=` figure close to or below 55 MBps -- either means this core
alone cannot hold the target frame rate against real scanout contention, which is exactly the answer
Stage 1 of the core-allocation plan (HE -> HP) or Stage 3 (A32 pair) exists to act on.

If Phase 3 never runs (`RESULT: display did not open after 5 attempts`), the Phase 1/2 numbers above it
are still valid and complete on their own -- that is the entire reason for the print-before-open ordering.

### Checksum

The last line, `checksum=0x........`, is not a result to interpret -- it exists so every buffer this
probe wrote was actually read back and consumed by something the compiler cannot prove is dead, which is
what stops `-O2` from deleting the measured loops and reporting an very fast, completely fictitious
number. A checksum is expected to differ between runs (it folds in `k_cycle_get_32()` as a seed); its
only job is to be present and non-degenerate.

## What this probe deliberately does NOT do

- **Does not touch the game.** Nothing under `src/`, `tests/`, `art/`, `tools/`, or the top-level
  `CMakeLists.txt` is read or written by anything here.
- **Bypasses `<alp/display.h>` for exactly one call pair.** `alp/display.h` has no blanking control, so
  Phase 3's blanked-vs-live measurement calls Zephyr's `display_blanking_on()` / `display_blanking_off()`
  directly on the raw device (the same precedent `examples/aen/aen-dsi-display` sets, for the same
  reason). Every pixel push still goes through `alp_display_blit()` -- the real, portable call a renderer
  would make. This is a small, logged SDK-surface gap, not something this probe works around silently.
- **Does not measure the camera or the IMU chain.** Out of scope for the fill-rate question.
