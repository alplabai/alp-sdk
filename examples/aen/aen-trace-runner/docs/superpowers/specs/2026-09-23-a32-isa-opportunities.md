# A32 ISA and micro-architecture opportunities: survey and bench plan (2026-09-23)

Scope: the dual Cortex-A32 renderer (AArch32, bare-metal BL33 under TF-A sp_min, MMU and L1/L2 on,
NEON). It runs at 30 Hz. The integrated build takes 19.4-29.5 ms/core of the 33.3 ms frame. The
question is which ARMv8-AArch32 features and build settings would give that time back. Nothing on
silicon has changed in this phase. The companion payload `a32/payload-isa` measures the points
that reading the code cannot settle; its recipe is at the end of this document.

The ground may move to GPU2D
(`trace-runner-hwa/docs/superpowers/specs/2026-09-23-gpu2d-ground-dma-design.md`). For that
reason the ranking puts wins in Gouraud spans, per-row overhead, triangle setup, sky/background
and scene build ahead of wins that only help the ground.

## The measured baseline (per core, per frame, 800 MHz: 1 ms = 800k cycles)

Source: `docs/2026-09-22-measurements.md`, sections P12, P16 and P16b. The profile is a
`make prof` renderer decoded with `a32/stub/decode.py --prof`.

| stage | ms/core | rate | notes |
|---|---|---|---|
| noz_tex (ground) | ~7.3 | ~21 cyc/px, 272k px | LDRB+LDRH gather, fogged palette |
| gouraud (z-tested) | ~3.0 | ~28 cyc/px, ~9 px/span | ~250 cyc/span incl. profile hooks |
| per-row + per-(tri,band) overhead | ~3.1 | ~134 cyc/row, ~18.7k rows | TRI counter minus the span counters |
| setup | 1.85-2.2 | ~1.5 us = ~1200 cyc/tri | 1209 tris/core |
| bg (z clear + sky + ground fill) | ~1.27 | 1.84 MB of stores/core | 20 bands x (46 KB z + 46 KB colour) |
| copy-out | ~0.46 | NC writes | DMA alternative measured 0.09, parked |
| bin | 0.16 | core 0 only | |
| scene build | ~1.7 (round 2; not re-measured since) | float front-end | split across both cores |

The clock is known: PMU against CNTVCT measured **800.0 MHz** (probe stage 2). The payload
measures it again.

The BG row checks out as pure store bandwidth: 1.84 MB/core in 1.27 ms is 1.45 GB/s. That matches
the dual-core per-core fill ceiling the probe measured (~1.43 GB/s). The sky's dither, halo and
stars cost almost nothing on top of it.

Caveat on the profile itself: every span pays `PROF_T0` + `PROF_ADD`. That means two out-of-line
calls (`tr_prof_now`, `tr_prof_core`), MRC PMCCNTR, MRC MPIDR and three read-modify-writes. With
~9 px spans, that cost is a large share of both the "gouraud" and the "per-row" figures. The
payload row `PROF_HOOK` gives cycles/hook. Multiply it by the span count (~one span per row) and
subtract it before trusting the 134 cyc/row figure. Compare stage times against the plain build,
as `renderer.c` already warns.

## Hot paths, read

- **Gouraud spans** (`r3d_raster.c` `span_gouraud_z`). Spans of n >= 8 use 8 px NEON chunks. The
  ragged end re-bases all four planes through `lanes()` (VLD1 + VMLA + VADD per plane) and runs one
  overlapped chunk that ends at n-1. n < 8 falls back to a scalar loop with a data-dependent
  z branch per pixel. With ~9 px spans the per-span work dominates, not the inner loop: 4 planes x
  (VDUP + VMLA + VADD) of setup, a second full chunk with a second `lanes()` set for n = 9..15, a
  branch on n < 8 that mispredicts on this mix, and (in the profile build) the hook. The overlapped
  chunk also reloads `dst[n-8..]` right after storing `dst[0..7]`. That is a partially overlapping
  store-to-load, which in-order cores typically do not forward. The payload times it.
- **Per row** (`raster_rows`). `e[]` and `pl[]` are local arrays walked by runtime-bounded loops
  (`nr`, `ne`), so every row loads and stores each edge's quot/rem/step/denom and each plane's row.
  On top of that come 4 multiplies for `sv[]`, the span call, and the plane-row adds. Each
  (triangle, band) entry also runs `raster_setup`: a copy of the setup record, `edge_dda_skip`
  (band-sized steps), the plane advance, and a dispatch on kind.
- **Setup** (`tri_setup`). All integer, deliberately, because it has to be bit-exact against the
  host goldens. The float/VDIV question does not arise here. The cost sits in: `edge()` int64
  products; up to 3 x `edge_dda_init_*`, each with 2 `floor_div` (32-bit SDIV fast path) plus 1
  SDIV in `edge_dda_band_step`; `R = 2^62 / area2`, a **uint64 division**, which is a libgcc
  `__aeabi_uldivmod` call on the A32; and 4 x `plane_init` (2 x `smul_shr` = 4 UMULL each, plus
  wrapping uint64 products). `strip_rows` adds int64 divides for triangles that cross the screen
  sides.
- **Binning** (`tr_bin_only`): two passes over the DL on core 0. It is small (0.16 ms).
- **Scene build** (`r3d_math.c` / `r3d_scene.c`, `tr_scene_build_part` on both cores). This is
  float: 2 VDIV per projected vertex (`p.x * f / p.z`, `p.y * f / p.z`), plus 1 in `w16_of` and 1
  in `vattr`, plus clip lerps. `-ffp-contract=off` is required for goldens.
- **Sky / bg** (`band_background`). This is a memset of the z band (newlib memset) plus a VST1Q
  row pattern of 8 px per store, per band. The ground rows get `tr_span_fill` of the ground colour,
  and then the NOZ ground triangles overwrite most of them.
- **Copy-out** (`copy_band`): VLD1/VST1 of 16 B from the cached band to the NC framebuffer.

## Opportunities, with estimates

Each estimate is grounded in the numbers above. "Bench" names the payload rows that settle it.

### 1. Gouraud: NEON for every length, lane-masked tail (bench `GZ_CUR_*`, `GZ_SCALAR_*`, `GZ_MASK_*`)

The candidate is `span_gouraud_z_masked` in `isa_bench.c`. It is checked bit-exact against the
scalar reference on host and qemu, and `GZ_MASK_EXACT` repeats the check on silicon. It sets up the
lanes once, then runs 8 px chunks. The last chunk is masked by `lane < n - i` (VCLT against an index vector, then VAND into the z
mask). There is no scalar path, no re-base and no overlapped chunk.

Cost: it reads and rewrites up to 7 px past the span end with the values it read. Inside a band
row that is safe, because the same core writes sequentially. At the very end of a core's colour
band it would touch the other core's z band. The layout therefore needs 8 px (16 B) of slack after
each band buffer, a memory-map change of 64 B.

The same scheme applies to NOZ Gouraud and flat-z. Estimate: 0.5-1.0 ms/core if the bench shows
`GZ_MASK_9` / `GZ_MASK_4` at roughly half of `GZ_CUR_9` / `GZ_CUR_4`. Per-span setup, not
pixels, is the real cost at 9 px. Effort: low; the kernel exists.

### 2. Per-row overhead: register-resident, edge-count-specialised row loop (bench `EDGE_CUR`, `EDGE_REG`, `EDGE_FIX`)

`edge_reg_n()` is the candidate. The `(nr, ne)` pair is one of `(1,2)`, `(1,3)`, `(2,3)`; with it
compile-time, the DDAs and planes stay in core registers and the loops unroll. It gives the same
bits (check `EDGE_REG_EXACT`) with no division and no int64.

The fixed-point variant (16.16 slope, `EDGE_FIX`) is **not exact**. qemu counts 2 of 4,273 rows
differing from the exact DDA, so it would break the golden band CRCs. That rules it out whatever
its speed. The exact rational DDA is already the cheap form: an add, an add, and a compare with
one conditional correction.

Estimate: 20-40 cyc/row off the ~100 that remain once the profile hook is subtracted, which is
0.5-0.9 ms/core. Row batching (2 rows per iteration, so the two edge steps' compares pair up)
could come after that; it is not benched. Effort: medium. It multiplies the `raster_rows` copies
by 3, so watch the image size (512 KiB budget; -O2 scene image is 408,512 B).

### 3. Background: store less, store wider (bench `FILL16K_*`, `FILL45K_*`)

BG runs at the fill ceiling, so the ISA lever is the store form. The bench compares newlib
memset, VST1Q (the current form), `VST1.64 {d0-d3}, [r:256]!` (32 B per instruction, aligned
hint), `VSTMIA {d0-d7}` (64 B) and STRD, on an L1-sized (16 KiB) and a band-sized (45 KiB)
buffer. If the best form beats the current one by ≥25 %, that is ~0.3 ms/core.

The structural levers are larger and sure, independent of the ISA:
- **(a)** Skip the colour fill of rows that the NOZ ground covers completely. The ground is drawn
  first (bin pass 0), and it is ~59 % of the screen. Estimate ~0.3-0.4 ms/core. If the ground
  moves to GPU2D this changes shape; see that design.
- **(b)** Clear z only over the rows (or the x-span) that z-tested triangles of the band touch. The
  bin pass already knows every band's triangles. Estimate 0.2-0.4 ms/core. Effort: low.

### 4. Setup: the uint64 divide, then plane batching (bench `UDIV_LAT`, `LDIV64`, `RECIP62_FAST`, `TRI_SETUP`)

`recip62()` computes `floor(2^62 / a)` bit-exactly as follows: a VFP double estimate (hardware
VCVT.F64.U32 x 2, VDIV.F64), then at most a few integer correction steps. Operands `a < 1024` use
the plain divide. `RECIP62_EXACT` checks it against `/` on 1,024 realistic `area2` values plus
every a < 5000. The saving is (`LDIV64` - `RECIP62_FAST`) x ~1209 tris; at ~100 cycles saved that
is ~0.15 ms/core. Effort: low. The golden CRCs guard it.

`TRI_SETUP` gives the isolated cycles/tri for character-sized Gouraud triangles, to set against
the ~1200 measured in context. The difference is cache misses on the DL and the setup records.

The remaining arithmetic is 4 x `plane_init`, all sharing `R`. It maps onto VMULL.S32 (2 lanes of
32x32->64) for nx/ny, but `smul_shr`'s 64x64 high product does not. Expect 0.2-0.4 ms/core for
medium-high effort. Do the divide first.

### 5. -O3 / -funroll-loops / LTO (bench: `GOLD_RASTER` in `isa-O2` vs `isa-O3` vs `isa-O3u`; renderer `make ROPT=...`)

The raster is integer-only, so any -O level is bit-exact by construction. `GOLDEN_OK` (the
per-band golden CRCs) proves it in the payload, and the renderer's golden build proves it on the
full image.

The float front-end is where flags matter. Result of the A32-qemu run of every `test_r3d_*.c`,
including `test_r3d_band.c`'s `TR_GOLDEN_FE_CRC` and the committed DL `memcmp`:

| extra flags (on top of the renderer's) | band (FE CRC + DL) | math | scene | raster, sky, zones, ent_lead | verdict |
|---|---|---|---|---|---|
| -O2 (shipped) | ok | ok | ok | ok | baseline |
| -O3 | ok | ok | ok* | ok | **golden-safe** |
| -O3 -funroll-loops | ok | ok | ok* | ok | **golden-safe** |
| -O2 -fno-math-errno | ok | ok | ok | ok | golden-safe |
| -O2 -ffinite-math-only | ok | ok | ok | ok | golden-safe on these tests; NaN/Inf guards become dead code, so not recommended |
| -O2 -fno-signed-zeros | ok | ok | ok | ok | golden-safe on these tests |
| -O2 -fno-trapping-math | ok | ok | ok | ok | golden-safe |
| -O2 -freciprocal-math | ok | ok | **FAIL** | ok | **breaks goldens** |
| -O2 -ffast-math | **FAIL** | **FAIL** | **FAIL** | ok | **breaks goldens** |
| -O2 -flto | ok | ok | ok | ok | golden-safe |
| -O3 -flto -fno-math-errno | ok | ok | ok | ok | golden-safe |

\* At -O3, `test_r3d_scene.c:371` trips `-Werror=maybe-uninitialized` in the TEST code (a false
positive on `at`). With `-Wno-error=maybe-uninitialized` it builds and passes. Fix the test before
flipping the renderer to -O3, or runner.sh's A32 stage will not build at -O3. Reproduce it like
this: for each flag set, build every `tests/host/test_r3d_*.c` with
`$A32_GCC $WARN <flags> -mcpu=cortex-a32 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard --specs=rdimon.specs`
plus runner.sh's `$R3D_SRC`, then run `qemu-arm -cpu max`. That is runner.sh's A32 stage with
the flags appended.

Sizes (renderer scene image, this tree): -O2 408,512 B; -O3 444,144 B; -O3 -funroll-loops
465,008 B; -O2 -flto 408,308 B; -O3 -flto 457,188 B. All fit the 512 KiB budget, but
-O3 -funroll-loops leaves ~59 KB.

`make ROPT=-O3` (a32/renderer/Makefile, new knob, default -O2) is the whole-renderer A/B. Run it
with `make prof ROPT=-O3` and the plain build on silicon. Estimate: unknown until measured;
typically 0-10 % on span loops that are already hand-vectorised. Effort: trivial. It ranks high on
effort alone.

### 6. VDIV vs VRECPE + 2 x VRECPS (bench `FDIV_*`, `RECP_*`, values `RECP_MISMATCH`/`RECP_MAX_ULP`)

This applies only to the float scene build; the raster has no float divide. Two facts decide it:
- The estimate is **not IEEE-exact**. qemu, whose VRECPE/VRECPS follow the architecture tables
  bit for bit, gives 1908 of 4096 projection-like quotients differing, by up to 2 ulp. That
  changes the DL, which breaks `TR_GOLDEN_FE_CRC` and the committed golden DL unless the host
  reference emulated the same sequence.
- The volume is small: ~4 divides per projected vertex, a few thousand vertices per frame, split
  across two cores.

Even if VDIV costs ~15-20 cycles against ~2-3 cycles/lane for a batched VRECPE sequence, the
ceiling is ≤0.1 ms/core. The M55's 4.8x does not transfer: there it replaced a non-pipelined
scalar divide in a hot loop. **Recommendation: do not.** The bench records the real A32 VDIV
latency and throughput so this stays closed.

### 7. FPSCR FZ/DN (bench `FMUL_NORM[_FZ]`, `FMUL_DENORM[_FZ]`)

The stub hands payloads FPSCR = 0: IEEE denormals, NaN propagation. Advanced SIMD in AArch32
always flushes to zero regardless. The bench shows whether the A32's VFP pays a denormal penalty.
Denormals essentially never occur in the camera-space and colour math, so the gain is ~0.
Setting FZ changes results only for denormal operands, but that is still a golden risk for no
gain. **Recommendation: leave FPSCR at 0.**

### 8. PLD on texture fetch (bench `TEX_WARM[_PLD]`, `TEX_COLD[_PLD]`)

A ground texture is 128x128 x 2 B = 32 KiB. The P15 zone textures are 2 x 32 KiB RGB565 plus
4 x 16 KiB indices (`tr_memmap.h`). All are L2-resident after the first frame, and mostly
L1-resident within a band. PLD can only hide the first-touch misses of a frame: ~512 lines per
32 KiB at roughly L2/SRAM latency, which is ≤0.05-0.1 ms/core. It is also ground-only.
**Low priority.** The bench shows the cold/warm delta and whether a 2-sub-span-ahead PLD recovers
it. PLDW on band lines does not apply either: the bands are rewritten every band and stay cached.

### 9. Cache-line alignment

- Setup records are 140 B, so a record straddles 3 lines. Padding to 192 B would not reduce the
  lines touched, and every field is used per band entry. No gain.
- DL triangles are 52 B. Band buffers are already 64 B aligned.
- One real layout question remains: the z + colour working set is 2 x 46 KB per core. If L1D is
  32 KiB (the payload reads CCSIDR), every band lives in L2. **Interleaving z and colour** (4 B/px,
  VLD2/VST2.16) would put a 9 px span's z and colour in one line instead of 2-4. Unknown gain,
  medium effort, needs a prototype; listed and not estimated.

### 10. L1/L2 prefetch controls, ACTLR/CPUACTLR/L2CTLR/L2ECTLR

TF-A never writes the Secure ACTLR: upstream `cortex_a32.S` only sets CPUECTLR.SMPEN, and
`tfa_alif` has no ACTLR write. The access-enable bits it would need for NS PL1 to reach CPUACTLR,
CPUECTLR, L2CTLR, L2ECTLR or L2ACTLR therefore stay at their reset values. Expect **UNDEF** from
NS; the payload records exactly which reads UNDEF (`undef_mask`) and dumps what is readable,
including CLIDR/CCSIDR, the real cache geometry. Changing prefetcher settings is a TF-A (secure)
change and out of scope here. The payload never writes these registers.

### 11. Branch prediction and code placement

Branch prediction is on. SCTLR = 0x00C5183D has Z (bit 11) set, and in ARMv8 AArch32 it is RES1
anyway. The stub invalidates the BTB at re-entry.

Code runs from SRAM1 0x025xxxxx as WB-WA, so it is cached; hot loops are small. The only placement
risk is -O3 / unroll growth thrashing the L1I; the payload reads its size from CCSIDR.
Nothing to gain.

### 12. Core-1 vs core-0 balance

Bands are claimed dynamically (LDREX/STREX), and measured raster time is balanced (core 0/1 frame
21.9-22.3 ms each in round 2). Idle time comes from core-0-only serial sections: scene step,
`render_front_end` (DL1 append memcpy of ~52 B/tri plus bg), and `render_bin` (0.16 ms). The setup
split is by count (n/2), not by cost. Estimate 0.1-0.3 ms/core:
- bin half the bands per core (-0.08);
- split setup by cost;
- have bins index two DLs instead of appending.

Measure first: `out_ticks0` against `out_ticks1` is already published per frame.

### 13. Clock

800.0 MHz, measured. Alif's published figure for the Ensemble Cortex-A32 is 800 MHz maximum (from
memory, not checked against the E8 datasheet in this phase; confirm there). The SE owns the clock tree, and no in-spec
headroom is known. **Report only; clocks untouched.**

### 14. Ground (noz_tex) software pipelining (ground-only)

The ground runs at 21 cyc/px against ~10 instructions/px. The in-order core stalls on the
dependent LDRB (index) -> LDRH (palette) chain. Computing the next pixels' offsets before
consuming this pixel's loads, or forming 8 offsets in NEON as `tex_run8` does, could reach
~12-14 cyc/px: **2.5-3 ms/core, but only if the ground stays on the A32.** GPU2D removes it
entirely. Park it until the GPU2D decision.

## Ranking (expected ms/core saved per unit of effort, ground-only last)

| # | opportunity | est. ms/core | effort | golden-safe | settled by |
|---|---|---|---|---|---|
| 1 | -O3 (-funroll-loops) A/B, `make ROPT=-O3` | 0-1 (unknown) | trivial | raster yes; front-end per table above | `GOLD_RASTER` x3 builds, renderer prof A/B |
| 2 | BG: skip ground-covered colour fill + clear z only where z-tested geometry lands | 0.5-0.8 | low | yes (same pixels) | design; `FILL*` for store form |
| 3 | Gouraud: masked-tail NEON for all n (+16 B band slack) | 0.5-1.0 | low | yes (checked) | `GZ_*` |
| 4 | Per-row: register-resident (nr,ne)-specialised row loop | 0.5-0.9 | medium | yes (checked) | `EDGE_*` |
| 5 | Setup: exact `recip62()` for the uint64 divide | ~0.15 | low | yes (checked) | `LDIV64`, `RECIP62_FAST`, `TRI_SETUP` |
| 6 | Band fill store form (VST1 :256 / VSTM) | 0-0.3 | low | yes | `FILL*` |
| 7 | Core balance: split bin, cost-split setup, no DL1 append | 0.1-0.3 | medium | yes | out_ticks0/1 |
| 8 | Setup plane batching (VMULL.S32) | 0.2-0.4 | medium-high | must re-prove | `TRI_SETUP` |
| 9 | LTO (scene build) | 0.05-0.2 | trivial | per table above | renderer pad3[5] A/B |
| 10 | Interleaved z/colour band (VLD2/VST2) | ? | medium | yes | prototype; CCSIDR L1D size |
| 11 | PLD on texture fetch | ≤0.1 (ground) | low | yes | `TEX_*` |
| 12 | VRECPE+VRECPS for scene divides | ≤0.1 | low | **no** (1908/4096 differ) | `RECP_*`, `FDIV_*` |
| 13 | FPSCR FZ/DN | ~0 | trivial | risk | `FMUL_*` |
| 14 | L2/L1 prefetch via ACTLR family | ? | TF-A change | yes | `undef_mask`; secure side |
| 15 | Clock | 0 (800 MHz, no known in-spec headroom) | n/a | n/a | `clk_*` |
| 16 | Ground noz_tex software pipelining | 2.5-3 (ground only) | medium | yes | park until the GPU2D decision |

The "profile hook" correction is not a speedup, but it changes how items 3 and 4 read. Take the
`PROF_HOOK` cycles/hook, multiply by ~18.7k spans/core, and subtract that from "per-row
overhead" before sizing item 4.

## Microbenchmark payload (`a32/payload-isa`)

- **Kernels.** `isa_bench.c` includes `src/render/r3d_raster.c` itself, so the "current" rows time
  the renderer's own static kernels. The candidates sit beside them and are bit-exact-checked
  against them.
- **Payload side.** `isa.c` does the rest: enables the PMU (the renderer's `prof_enable()`
  sequence), measures the clock (PMCCNTR over a 10 ms CNTVCT window), reads CP15 registers (an
  UNDEF is counted and skipped by `vectors.S`), and writes everything into the NC block at
  **0x02401C00** (256 words).
- **Memory.** 0x02401C00 lies clear of `tr_mbox_t` 0x000-0x1FF, prof 0x800-0x8DF, stats
  0x900-0x93F, the accel probe 0x02401A00-0x02401A8F, and the renderer L2 tables at 0x02402000.
  The results struct is asserted ≤ 0x400 B. `isa.ld` lets .bss run to 0x025F0000, the halted
  renderer's DL/bin/stack tail; the renderer rebuilds all of that at LAUNCH. The renderer's core-1
  gate word at 0x025F4000 is not touched.
- **Scope of the run.** Core 0 only. Everything runs once, well under 1 s, then control returns to
  the stub. It writes no clocks, no ACTLR family registers and no framebuffer. For the cold-texture rows it
  evicts the caches by reading 1 MiB at 0x02600000 (FB B, WB in the stub table, read only, so no
  line is dirtied).

Measured on core 0 alone, with core 1 parked:
- The fill rows are single-core rates. The renderer's dual-core fill ceiling is ~1.43 GB/s/core,
  not ~1.9.
- The golden frame is the 287-tri CP-A6 DL, not the live scene. `GOLD_RASTER` is for A/B between
  builds, not an absolute frame cost.
- The payload's bands sit in SRAM1 (WB); the renderer's are in SRAM0 0x022A8000 (WB), so latency
  may differ slightly.

### Builds (sizes/CRCs at this commit)

| build | command | ctrl_len | ctrl_crc |
|---|---|---|---|
| -O2 | `make -C a32/payload-isa` | 50,672 B | 0x89C457EE |
| -O3 | `make -C a32/payload-isa OPT=3` | 62,904 B | 0x27FA7D18 |
| -O3 -funroll-loops | `make -C a32/payload-isa OPT=3 UNROLL=1` | 84,280 B | 0x0FDB44B9 |

Each build writes `isa-O<n>[u].bin`, `isa-O<n>[u]-launch.jlink` (from `mkpayload.py`: HALT, a
bounded stub_state poll, `loadbin ..., 0x02500000, noreset`, then ctrl_entry/len/crc and LAUNCH)
and `halt.jlink`. `make -C a32/payload-isa check` runs the kernels on the host cc and under
qemu-arm (NEON) with every bit-exact check required. `tests/host/runner.sh` runs it in its A32
stage.

### Bench recipe (2026W36-0009, dev loop; the orchestrator runs this, not an implementor)

1. The dev loop must be up: A32 stub resident (`mem32 0x02401000, 1` = 0x54524D42), HE running.
   The payload does not need the HE to be idle. The display keeps scanning the renderer's last FB,
   because the payload never writes a framebuffer.
2. `JLinkExe -SelectEmuBySN <2026W36-0009 probe> -CommandFile a32/payload-isa/isa-O2-launch.jlink`. In
   the log, the last `mem32 0x02401190` before `loadbin` must read 0 (PARKED) or 2 (FAULT), never 1.
3. After ~1 s, read the block with a J-Link command file containing
   `si SWD / speed 4000 / device Cortex-M55 / connect / mem32 0x02401C00, 256 / mem32 0x02401180, 5 / mem32 0x024011C0, 8 / exit`,
   and save the output as `isa-O2.txt`. `stub_state` (0x02401190) must be 0 (PARKED; the payload
   returned). A FAULT means `fault_*` at 0x024011C0 has the record; decode it with
   `a32/stub/decode.py`.
4. `python3 a32/payload-isa/decode_isa.py isa-O2.txt`. Required: `stage=0xD0E (done)`, every check
   `pass`, and the clock at ~800 MHz.
5. Repeat steps 2-4 with `isa-O3-launch.jlink` and `isa-O3u-launch.jlink`. The A/B rows are
   `GOLD_RASTER`, `GZ_*` and `EDGE_*`.
6. Relaunch the renderer: `a32/renderer/renderer-launch.jlink` (or the release image's own
   autolaunch after a cold cycle).
7. Optional whole-renderer A/B: `make -C a32/renderer ROPT=-O3` and `make -C a32/renderer prof
   ROPT=-O3`. Launch each, then read `out_ticks0` / pad3 / `decode.py --prof`, and compare against
   the -O2 images at the same LOD. Run the golden build (`V=golden ROPT=-O3`) too: its band CRC
   counters (pad3[6]) must show 0 mismatches.

Record the results as a new section of `docs/2026-09-22-measurements.md`, then re-rank the table
above from the measured rows.

## Silicon results and what landed (2026-09-23)

The payload ran on 2026W36-0009 at -O2, -O3 and -O3u (the latter = -O3 -funroll-loops). All 9 checks
passed, the clock read 800.0 MHz, and no register read UNDEFed:

- ACTLR 0
- L2ACTLR 0x80000008
- CPUACTLR 0x090CA000
- CPUECTLR 0x40
- L1D 32 KiB, L1I 32 KiB, L2 512 KiB

On this evidence PLD (it hurt: 12.26 -> 13.48 cyc/px), VRECPE (not exact) and FZ (no denormal
penalty) were dropped.

Per-frame counts used below come from a 160-frame host model of the scene: ~18.7k rows/core,
~13.2k z-tested Gouraud spans/core (9.9k shorter than 8 px, 2.0k of 8-15 px, 1.3k of 16 px or
more), ~1.2k tris/core, and 1.84 MB of band fill/core. The estimates are single-core bench rates
multiplied by those counts, so they are upper bounds under dual-core memory contention.

| commit | change | silicon unit cost | est. ms/core |
|---|---|---|---|
| 100fca8 | r3d_raster.c at -O3 -funroll-loops (the rest stays -O2: image-wide, __bss_end sat 1.9 KB under the DL) | rows 137.6 -> 107.2 cyc; setup 1661 -> 1414 cyc/tri; spans <8 px 130 -> 79 cyc | ~1.8 |
| b6bc21f | masked last NEON chunk (n >= 8), used only while it stays inside the row, so no band slack | 8-15 px 255 -> 193 cyc; 16+ 205 -> 186 | ~0.19 |
| f9abad4 | row loop in locals, (nr, ne) constant for Gouraud / NOZ Gouraud / flat | 107.2 -> 75.6 cyc/row | ~0.65 |
| 6b674e8 | z clear + background as VSTM | 0.240 / 0.188 -> 0.105 cyc/B | ~0.25 |
| 5bb85aa | recip62() for the setup divide | 121 -> 84 cyc/tri | ~0.06 |
| 484aa51 | decode.py --prof prints overhead with the ~32 cyc/span hook removed | — | (measurement) |

Declined, by measurement:
- **Partial z clear.** 44 % of band rows could be skipped, which is ~0.05 ms/core at the VSTM
  rate, but it would leave z undefined outside those rows, and tests read z.
- **Ground colour-fill skip.** ~0.06 ms/core, and not provably exact without per-row coverage
  tracking.
- **Core-balance tweaks.** The dynamic band claim already balances the raster. Splitting the bin
  saves ≤0.08 ms and needs another cross-core sync.

Next measurement for the orchestrator: short spans (<8 px, 75 % of Gouraud spans) now dominate
Gouraud time at ~79 cyc each. A branch-free scalar form is the next candidate to bench.

## Phase 3: algorithm study items (spike/algo f958546)

| commit | change | study est. ms/core | exactness evidence |
|---|---|---|---|
| 34bd865 | ground span (`span_tex`, `pl[W].gx == 0`): `persp_w()` (the UDIV, CLZ, normalisation) and the fog level + palette row computed once per span; the sub-span loop specialised per palette or plain texture | 0.8-1.1 (#1) | the selfcheck gives odd-length textured spans gx_w = 0; injected bugs fail test_r3d_band/zones; 160-frame scene A/B identical |
| c217395 | `tri_setup()` returns before the DDAs and planes when the closed bounding box holds no pixel centre (host model: ~394 tris a frame) | ~0.1 (#10) | over-rejecting one-column tris fails test_r3d_scene/raster; A/B identical |

Payload rows `GSPAN_OLD` / `GSPAN_CW` time a 720-px fogged constant-w ground row through the
generic sub-span loop and through `span_tex()`'s hoisted loop.

Tried and reverted: Gouraud-z lane vectors kept across a triangle's rows (study #3; 4c601d7,
reverted by 7075971). On silicon it gained nothing (GZR_9 27.3 vs GZ_CUR_9 27.1 cyc/px), and NEON
for short spans lost (GZH_4 42.7 vs GZ_CUR_4 19.7 cyc/px).

Not done: the spill-free ground texel run (study #6). After the hoist, the fogged constant-w
sub-span loop still makes ~20 stack references per 8 px (static count in the -O3u listing: 110
instructions, 8 LDRB, no divide). Removing them needs hand-written assembly, which stays parked
until the GPU2D ground decision.

## Silicon: the integrated build (2026W36-0009, spike/a32-isa 76f91e6)

- End-to-end frame: **15.3-24.3 ms/core, mean 19.3**, against feat/core's 19.4-29.5.
- `make prof` unit rates, against feat/core:
  - noz_tex 23.9 -> 16.5 cyc/px;
  - bg 1.9 -> 1.2 ms;
  - setup 1261 -> 1002 cyc/tri;
  - Gouraud 26.2 -> 22.2 cyc/px.
- Golden build on silicon: pad3[6] [31:16] = 0 mismatches, so the raster is bit-exact.
- payload-isa -O3u: 11/11 checks pass.

Fix round after review:
- the lane-vector commit is reverted;
- `TR_RASTER_CHECKS` asserts the masked chunk's row bound (812d822);
- `recip62` takes one proved correction step (7777394);
- the DL moved to SRAM0 0x021C2000, so the renderer image + .bss may reach 0x025C0000
  (6db1a8b).
