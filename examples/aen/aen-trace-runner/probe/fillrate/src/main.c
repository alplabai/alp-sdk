/* probe/fillrate/src/main.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stage-0 fill-rate probe -- see docs/2026-09-22-core-allocation.md for why
 * this exists and what decision rides on it. Standalone from the game: it
 * shares no source with src/, only alp-sdk and Zephyr.
 *
 * THE QUESTION: can a Cortex-M55 sustain a full-screen repaint of the
 * 720x1280 RGB565 panel at a playable frame rate, and where does it
 * saturate? The CDC200 scans out continuously at 40 MHz (~80 MB/s of SRAM
 * reads that never stop), so the interesting number is bandwidth UNDER that
 * contention, not bandwidth in a vacuum -- Phase 3 measures both and prints
 * the difference explicitly.
 *
 * ORDERING IS LOAD-BEARING: the panel's init fails on roughly 1 cold boot in
 * 8-10 with no re-init path (see trace-runner's own src/platform/display.c).
 * Every measurement that does not need the display (Phases 1-2) runs and
 * prints BEFORE alp_display_open() is attempted (Phase 3), so a run where
 * the panel never comes up still yields the memory numbers.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <arm_mve.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>

#include <alp/display.h>
#include <alp/peripheral.h>

/* ----------------------------------------------------------------------
 * Timing source.
 *
 * k_cycle_get_64() reads the kernel's free-running HW cycle counter --
 * resolution is 1 cycle at CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC (printed at
 * startup via sys_clock_hw_cycles_per_sec(), not assumed). It wraps roughly
 * every 26.8 s at 160 MHz / 10.7 s at 400 MHz. Every timed section here is a
 * single (t1 - t0) computed with unsigned 32-bit subtraction, which is
 * correct across one wrap, and every section below completes in well under
 * a second -- see examples/aen/aen-rtc-tick-probe for the case (a
 * multi-second window) where a 64-bit counter would be needed instead.
 * ------------------------------------------------------------------- */

/* Panel geometry (must match the shield's cdc200 + panel nodes; checked
 * against alp_display_get_caps() before Phase 3 trusts it). */
#define PANEL_W 720u
#define PANEL_H 1280u
#define PANEL_BPP 2u /* RGB565 */
#define PANEL_FRAME_BYTES (PANEL_W * PANEL_H * PANEL_BPP) /* 1,843,200 B */

/* ----------------------------------------------------------------------
 * Phase 1 -- raw memory bandwidth, no display.
 *
 * SRAM0 (shield-shrunk to the 2 MiB below lcd_fb) and SRAM1 (4 MiB, wholly
 * unused by this probe) are global on-chip banks, not the kernel's own SRAM
 * region here (zephyr,sram = &dtcm -- see the board overlay), so there is no
 * linker-declared C object to point at: address them directly by
 * devicetree reg, the same pattern aen-hp-core-smoke uses for its SRAM0
 * liveness beacon (it writes that bank "by absolute address").
 *
 * WORKING-SET SWEEP, not a single fixed size: the Cortex-M55's D-cache is
 * configurable from 4 KiB to 64 KiB, and neither alp-sdk nor the Zephyr SoC
 * files record what Alif set it to for the E8. A single 64 KiB buffer would
 * have silently measured cache bandwidth and reported it as SRAM bandwidth
 * if the configured size turned out to be 64 KiB -- a confidently wrong
 * number is worse than none, so this sweeps the size instead of assuming
 * one. The sweep spans the entire possible D-cache range (4/16/64 KiB) and
 * continues on past it (256/1024 KiB) towards the 1,843,200 B framebuffer's
 * actual scale, so the same data set shows: the cached rate at the small
 * end (if any), the sharp drop where the working set stops fitting (the
 * cache size itself, as a free by-product), and the true sustained SRAM
 * rate at the large end -- which is the number that matters for a
 * framebuffer-sized workload. See README.md for how to read the cliff.
 * ------------------------------------------------------------------- */

struct sweep_step {
	uint32_t kib;
	uint32_t iters; /* chosen so every step moves roughly the same total bytes -- keeps the
			 * timed window comparable across sizes without the smallest steps being
			 * too short to time accurately or the largest too slow. */
};

static const struct sweep_step SWEEP[] = {
	{ 4,    64 },
	{ 16,   32 },
	{ 64,   16 }, /* the top of the M55's entire possible D-cache range */
	{ 256,   4 },
	{ 1024,  2 }, /* approaching the framebuffer's 1,843,200 B */
};
#define SWEEP_N ((uint32_t)ARRAY_SIZE(SWEEP))

/* DTCM's whole bank is only 256 KiB on the HE core (see below) -- it
 * physically cannot host the 256/1024 KiB steps alongside a second buffer,
 * the stack and the RAM console, so it only runs the first 3 (<=64 KiB)
 * sweep entries. That is a hard capacity limit, not a shortcut: the
 * framebuffer itself lives in SRAM (SRAM0's lcd_fb region), not DTCM, so
 * SRAM0/SRAM1's full sweep is what answers the framebuffer-scale question;
 * DTCM only needs to answer "does its cache-range behaviour match SRAM's". */
#define SWEEP_DTCM_N 3u

#define SRAM0_NODE DT_NODELABEL(sram0)
#define SRAM1_NODE DT_NODELABEL(sram1)

/*
 * SRAM1 is OFF by default: it is power-gated and unpowered at boot.
 *
 * On E1M-AEN803 2026W36-0009, measured cold with no image loaded, SRAM1 does
 * not answer -- 0x023FFFF0 (the top of SRAM0) reads back, 0x02400000 returns
 * "Failed to read memory" to the debugger as well as bus-faulting the core.
 * This probe's first bench run bus-faulted at exactly the SRAM0-to-SRAM1
 * handoff, so Phase 2 and Phase 3 never ran and the contention delta -- the
 * one number this probe exists to produce -- was never measured.
 *
 * That delta does not need SRAM1 at all: it is a fill of the panel's own
 * framebuffer, which lives in SRAM0. So with this off, Phase 1 sweeps SRAM0
 * and DTCM only, and FB_SCRATCH moves into SRAM0 (see below). Set it to 1
 * once SRAM1 is powered by a Secure-Enclave memory request, to add SRAM1's
 * own sweep.
 */
#ifndef PROBE_SRAM1
#define PROBE_SRAM1 0
#endif

/* Two 1024 KiB slices (the largest sweep step) at the bottom of each bank:
 * +0x000000 / +0x100000, ending exactly at +0x200000 with zero slack --
 * SRAM0's lcd_fb starts there (shield addressing) and SRAM1's FB_SCRATCH
 * (below) is placed there on purpose, so this sizing can't silently grow
 * into either without the BUILD_ASSERTs below catching it. */
#define SRAM_SWEEP_MAX_BYTES (1024u * 1024u)
#define FB_SCRATCH_OFFSET    0x200000u

#define SRAM0_A ((uint32_t *)(DT_REG_ADDR(SRAM0_NODE) + 0x000000u))
#define SRAM0_B ((uint32_t *)(DT_REG_ADDR(SRAM0_NODE) + 0x100000u))
#define SRAM1_A ((uint32_t *)(DT_REG_ADDR(SRAM1_NODE) + 0x000000u))
#define SRAM1_B ((uint32_t *)(DT_REG_ADDR(SRAM1_NODE) + 0x100000u))

BUILD_ASSERT((2u * SRAM_SWEEP_MAX_BYTES) <= DT_REG_SIZE(SRAM0_NODE),
	     "phase-1 SRAM0 sweep buffers (A+B at the top sweep size) must fit the shield-shrunk 2 MiB bank");
#if PROBE_SRAM1
BUILD_ASSERT((2u * SRAM_SWEEP_MAX_BYTES) <= FB_SCRATCH_OFFSET,
	     "phase-1 SRAM1 sweep buffers (A+B at the top sweep size) must stay clear of FB_SCRATCH");
#endif

/* DTCM *is* the kernel's own SRAM here (zephyr,sram = &dtcm), so this is an
 * ordinary static allocation the linker places and bounds-checks at link
 * time -- 128 KiB total (2 x the 64 KiB top-of-range DTCM sweep step),
 * safely inside the HE core's 256 KiB DTCM (the smaller of the two cores;
 * HP has 1024 KiB) alongside the 8 KiB RAM console, the 8 KiB main stack
 * and this file's other statics. */
#define DTCM_SWEEP_MAX_BYTES (64u * 1024u)
static uint32_t dtcm_a[DTCM_SWEEP_MAX_BYTES / 4u];
static uint32_t dtcm_b[DTCM_SWEEP_MAX_BYTES / 4u];

/* Framebuffer-sized scratch (1,843,200 B) for Phase 2's sprite-blit
 * destination and Phase 3's blit source -- 2 MiB into SRAM1, exactly where
 * the sweep buffers above end. */
#if PROBE_SRAM1
#define FB_SCRATCH ((uint16_t *)(DT_REG_ADDR(SRAM1_NODE) + FB_SCRATCH_OFFSET))
BUILD_ASSERT((FB_SCRATCH_OFFSET + PANEL_FRAME_BYTES) <= DT_REG_SIZE(SRAM1_NODE),
	     "FB_SCRATCH must fit SRAM1's 4 MiB bank alongside the phase-1 sweep buffers");
#else
/* With SRAM1 unpowered, FB_SCRATCH reuses SRAM0's Phase-1 sweep buffers.
 * Safe because the phases run strictly in sequence: Phase 1 has finished,
 * and its checksum has already been mixed, before Phase 2 writes the first
 * byte here -- nothing ever reads Phase 1's data back. This probe carries
 * only the display shield, so there is no camera video pool in SRAM0 to
 * collide with. */
#define FB_SCRATCH ((uint16_t *)SRAM0_A)
BUILD_ASSERT(PANEL_FRAME_BYTES <= DT_REG_SIZE(SRAM0_NODE),
	     "FB_SCRATCH must fit the shield-shrunk SRAM0 bank below lcd_fb");
#endif

/* Phase-1 table: one (fill target, copy dst=a/src=b, sweep length) triple
 * per region under test. File-scope so the `static const` instance in
 * main() names a type with external linkage, not a function-local one. */
struct region {
	const char *name;
	uint32_t   *a;
	uint32_t   *b;
	uint32_t    sweep_n; /* how many of SWEEP[]'s entries this region's capacity supports */
};

static uint32_t g_checksum; /* mixed from every buffer this probe writes; printed once at the end */

/* Fold every word of [buf, buf+words) into the running checksum. This is
 * what makes the writes above impossible to optimise away: the compiler
 * cannot prove the fill/copy loops are dead once their output feeds a value
 * that is later printed (see main()'s closing line). Deliberately NOT
 * volatile -- volatile would also block the store-widening/vectorisation a
 * real renderer's build gets, which would make the reported MB/s an
 * artificially pessimistic number instead of the true achievable one. */
static void mix_checksum(const uint32_t *buf, uint32_t words)
{
	uint32_t acc = g_checksum;

	for (uint32_t i = 0; i < words; i++) {
		acc ^= buf[i];
		acc = (acc << 1) | (acc >> 31); /* rotate so a repeated value doesn't cancel itself out */
	}
	g_checksum = acc;
}

/* Sequential 32-bit fill, `iters` passes over the same buffer inside the
 * timed window (more bytes moved per window = timer overhead matters less).
 * One untimed warm-up pass precedes it and is discarded. The per-iteration
 * seed varies so the loop cannot be recognised as writing a compile-time
 * constant and folded away. */
static uint64_t timed_fill(uint32_t *buf, uint32_t words, uint32_t seed, uint32_t iters)
{
	uint64_t t0, t1;

	for (uint32_t i = 0; i < words; i++) {
		buf[i] = seed ^ i;
	}
	t0 = k_cycle_get_64();
	for (uint32_t it = 0; it < iters; it++) {
		uint32_t s = seed + it;

		for (uint32_t i = 0; i < words; i++) {
			buf[i] = s ^ i;
		}
	}
	t1 = k_cycle_get_64();
	return t1 - t0;
}

/* Sequential copy, same warm-up/iters structure as timed_fill(). src's
 * content is set once (untimed) before timing starts -- the bytes copied
 * per pass don't need to change for a bandwidth measurement, and src/dst are
 * two distinct memory objects the compiler cannot merge or elide. */
static uint64_t timed_copy(uint32_t *dst, uint32_t *src, uint32_t words, uint32_t seed, uint32_t iters)
{
	uint64_t t0, t1;

	for (uint32_t i = 0; i < words; i++) {
		src[i] = seed ^ (i * 2654435761u);
	}
	for (uint32_t i = 0; i < words; i++) {
		dst[i] = src[i];
	}
	t0 = k_cycle_get_64();
	for (uint32_t it = 0; it < iters; it++) {
		for (uint32_t i = 0; i < words; i++) {
			dst[i] = src[i];
		}
	}
	t1 = k_cycle_get_64();
	return t1 - t0;
}

/* Helium (MVE) variants of timed_fill()/timed_copy(): one 128-bit VSTRW per
 * four words, the widest store the M55 issues -- the ceiling a renderer's
 * span/clear loops can reach from one core.  `words` must be a multiple of 4
 * and buf 16-byte aligned (both hold for the SRAM0 sweep buffers). */
static uint64_t timed_fill_mve(uint32_t *buf, uint32_t words, uint32_t seed, uint32_t iters)
{
	uint64_t t0, t1;

	t0 = k_cycle_get_64();
	for (uint32_t it = 0; it < iters; it++) {
		uint32x4_t v = vdupq_n_u32(seed + it);

		for (uint32_t i = 0; i < words; i += 4u) {
			vst1q_u32((void *)&buf[i], v);
		}
	}
	t1 = k_cycle_get_64();
	return t1 - t0;
}

static uint64_t timed_copy_mve(uint32_t *dst, const uint32_t *src, uint32_t words, uint32_t iters)
{
	uint64_t t0, t1;

	t0 = k_cycle_get_64();
	for (uint32_t it = 0; it < iters; it++) {
		for (uint32_t i = 0; i < words; i += 4u) {
			vst1q_u32((void *)&dst[i], vld1q_u32((const void *)&src[i]));
		}
	}
	t1 = k_cycle_get_64();
	return t1 - t0;
}

/* Every derived-number line goes through here: primitives first (working-set
 * size, iters, cycles), the derived MB/s last, and a plausibility flag
 * against the SoC's theoretical bus ceiling (clock * 4 B/cycle for a
 * single-cycle 32-bit SRAM access -- generous on purpose, so a figure at or
 * above it is real cause for suspicion, not just "fast"). `kib` is the
 * per-pass working-set size (0 = not a sweep line, e.g. Phase 3's full
 * framebuffer fill) -- the whole point of Phase 1's sweep is that this
 * column, not just the MB/s one, is what a reader scans for the cache
 * cliff. */
static void report_line(const char *region, const char *op, uint32_t kib, uint32_t bytes_total, uint32_t iters,
			 uint64_t cycles, uint32_t hz, uint32_t ceiling_mbps)
{
	uint64_t bps  = cycles ? ((uint64_t)bytes_total * hz) / cycles : 0;
	uint32_t mi   = (uint32_t)(bps / 1000000ull);
	uint32_t mf   = (uint32_t)((bps / 10000ull) % 100ull);

	if (kib != 0u) {
		printk("%-6s %-5s %5uKiB iters=%3u cycles=%10llu MBps=%4u.%02u", region, op, kib, iters, (unsigned long long)cycles, mi,
		       mf);
	} else {
		printk("%-6s %-5s bytes=%9u iters=%3u cycles=%10llu MBps=%4u.%02u", region, op, bytes_total, iters,
		       (unsigned long long)cycles, mi, mf);
	}
	if (mi >= ceiling_mbps) {
		printk("  ** >= theoretical peak %u MBps -- suspect, do not trust **", ceiling_mbps);
	}
	printk("\n");
}

/* ----------------------------------------------------------------------
 * Phase 2 -- the renderer's actual inner loops, still no display.
 * ------------------------------------------------------------------- */

#define SPR_W     96u
#define SPR_H     96u
#define SPR_BYTES (SPR_W * SPR_H / 2u) /* 4 bpp packed, 2 px/byte = 4,608 B */
#define SPR_ITERS 200u

static uint8_t  sprite[SPR_BYTES];
static uint16_t palette[16];

static void init_sprite(uint32_t seed)
{
	for (uint32_t i = 0; i < SPR_BYTES; i++) {
		sprite[i] = (uint8_t)(seed ^ (i * 131u));
	}
	for (uint32_t i = 0; i < 16u; i++) {
		palette[i] = (uint16_t)(seed * (i + 1u) + i);
	}
}

/* One 96x96 blit through the 16-entry palette into `dst` (stride
 * `dst_stride` pixels), top-left corner. Position doesn't matter for a
 * throughput measurement -- every iteration does the same amount of work. */
static inline void sprite_blit_once(uint16_t *dst, uint32_t dst_stride)
{
	for (uint32_t y = 0; y < SPR_H; y++) {
		uint16_t *row = dst + y * dst_stride;

		for (uint32_t x = 0; x < SPR_W; x++) {
			uint8_t  byte = sprite[(y * SPR_W + x) >> 1];
			uint32_t idx  = (x & 1u) ? (byte >> 4) : (byte & 0x0Fu);

			row[x] = palette[idx];
		}
	}
}

static uint64_t timed_sprite_blit(uint16_t *dst, uint32_t dst_stride, uint32_t iters)
{
	uint64_t t0, t1;

	sprite_blit_once(dst, dst_stride); /* warm-up, discarded */
	t0 = k_cycle_get_64();
	for (uint32_t n = 0; n < iters; n++) {
		sprite_blit_once(dst, dst_stride);
	}
	t1 = k_cycle_get_64();
	return t1 - t0;
}

/* Horizontally-scaled scanline copy: the per-scanline op a flat-ground
 * perspective renderer performs -- read one texture row, write it stretched
 * to a fixed destination width. `src_w` varies (a near ground row samples a
 * narrow strip of texture and stretches it hard; a far row samples nearly
 * the full texture width and barely stretches at all); `dst_w` is always
 * one screen row. Nearest-neighbour sampling via a Q16 fixed-point step,
 * the standard renderer technique -- no float, no division per pixel. */
#define SRC_ROW_MAX_W 720u
#define SCANLINE_ITERS 2000u

static uint16_t src_row[SRC_ROW_MAX_W];
static uint16_t dst_row[PANEL_W];

static void init_src_row(uint32_t seed)
{
	for (uint32_t x = 0; x < SRC_ROW_MAX_W; x++) {
		src_row[x] = (uint16_t)(seed ^ x);
	}
}

static inline void scanline_once(uint32_t src_w, uint32_t dst_w, uint32_t step_q16)
{
	uint32_t frac = 0;

	for (uint32_t x = 0; x < dst_w; x++) {
		dst_row[x] = src_row[frac >> 16];
		frac += step_q16;
	}
}

static uint64_t timed_scanline(uint32_t src_w, uint32_t dst_w, uint32_t iters)
{
	uint32_t step_q16 = (src_w << 16) / dst_w;
	uint64_t t0, t1;

	scanline_once(src_w, dst_w, step_q16); /* warm-up, discarded */
	t0 = k_cycle_get_64();
	for (uint32_t n = 0; n < iters; n++) {
		scanline_once(src_w, dst_w, step_q16);
	}
	t1 = k_cycle_get_64();
	return t1 - t0;
}

/* ----------------------------------------------------------------------
 * Phase 3 -- with the panel live.
 *
 * alp/display.h has no blanking control (an SDK-surface gap: this probe's one
 * deliberate exception to consuming alp-sdk through its public headers only
 * -- logged here rather than worked around silently). alp_display_open()'s
 * zephyr backend already
 * calls display_blanking_off() internally at open (src/backends/display/
 * zephyr_drv.c), so scanout is already running the moment open() returns;
 * getting it stopped again for the "blanking on" measurement needs the raw
 * Zephyr device. examples/aen/aen-dsi-display sets the same precedent: it
 * drives <zephyr/drivers/display.h> directly for this exact reason. Every
 * pixel push below still goes through alp_display_blit() -- the real,
 * portable call a renderer would make -- so only the blanking toggle itself
 * bypasses the alp surface.
 * ------------------------------------------------------------------- */

#define DISP_OPEN_RETRIES 5   /* mirrors src/main.c's TR_DISPLAY_OPEN_RETRIES */
#define DISP_RETRY_MS     200 /* mirrors src/main.c's TR_DISPLAY_RETRY_MS */
#define FILL_WARM_ITERS   2u
#define FILL_TIMED_ITERS  20u
#define REPAINT_FRAMES    120u

static alp_display_t *open_display_with_retry(void)
{
	alp_display_config_t cfg = ALP_DISPLAY_CONFIG_DEFAULT(0);

	for (int attempt = 0; attempt < DISP_OPEN_RETRIES; attempt++) {
		alp_display_t *d = alp_display_open(&cfg);

		if (d != NULL) {
			return d;
		}
		/* Known ~1-in-8-to-10 cold-boot defect, no re-init path (see
		 * src/platform/display.c) -- retry a few times, a short beat apart. */
		printk("display: open attempt %d/%d failed (known intermittent panel init defect)\n", attempt + 1,
		       DISP_OPEN_RETRIES);
		if (attempt + 1 < DISP_OPEN_RETRIES) {
			k_msleep(DISP_RETRY_MS);
		}
	}
	return NULL;
}

int main(void)
{
	uint32_t hz           = sys_clock_hw_cycles_per_sec();
	uint32_t ceiling_mbps = (hz * 4u) / 1000000u; /* clock * 4 B/cycle: generous single-cycle-access ceiling */
	uint32_t seed         = k_cycle_get_32() ^ (uint32_t)(uintptr_t)&seed;

	printk("\n=== fillrate probe: %s ===\n", CONFIG_BOARD_TARGET);
	printk("clock: sys_clock_hw_cycles_per_sec()=%u Hz  (theoretical bus peak ~%u MBps)\n", hz, ceiling_mbps);
	printk("timing: k_cycle_get_64(), 1-cycle resolution, no practical wrap\n");
	/* Cache state up front, next to core identity and clock: a reader's first question about
	 * any surprising Phase 1 number is "was cache on", and this is where they'll look. dcache
	 * OFF is the expected/intended state (SoC-wide erratum: SCB_EnableDCache hangs on this
	 * silicon -- see prj.conf) -- Phase 1's sweep below is what actually PROVES it behaves that
	 * way, rather than trusting this Kconfig readout alone. */
	printk("icache: %s   dcache: %s%s\n", IS_ENABLED(CONFIG_ICACHE) ? "ON" : "OFF",
	       IS_ENABLED(CONFIG_DCACHE) ? "ON" : "OFF",
	       IS_ENABLED(CONFIG_DCACHE) ? " (unexpected -- verify prj.conf)" : "");

	/* ---------------- Phase 1: raw memory bandwidth, no display -------- */
	printk("\n--- phase 1: raw memory bandwidth, working-set sweep (spans the M55's whole\n");
	printk("possible D-cache range 4-64 KiB, then on to framebuffer scale) ---\n");
	printk("(copy runs at the smallest+largest size only, to fit the console budget; fill spans all sizes)\n");
	printk("(copy's KiB is the payload moved one direction per pass; actual bus traffic is ~2x that)\n");
	{
		static const struct region regions[] = {
			{ "SRAM0", SRAM0_A, SRAM0_B, SWEEP_N      },
#if PROBE_SRAM1
			{ "SRAM1", SRAM1_A, SRAM1_B, SWEEP_N      },
#endif
			{ "DTCM",  dtcm_a,  dtcm_b,  SWEEP_DTCM_N },
		};

		for (size_t i = 0; i < ARRAY_SIZE(regions); i++) {
			const struct region *r = &regions[i];

			for (uint32_t s = 0; s < r->sweep_n; s++) {
				uint32_t words = (SWEEP[s].kib * 1024u) / 4u;
				uint32_t iters = SWEEP[s].iters;
				uint32_t seedv = seed ^ (uint32_t)(uintptr_t)r->a ^ SWEEP[s].kib;
				uint64_t cyc   = timed_fill(r->a, words, seedv, iters);

				mix_checksum(r->a, words);
				report_line(r->name, "fill", SWEEP[s].kib, words * 4u * iters, iters, cyc, hz,
					    ceiling_mbps);

				if (s == 0u || s == r->sweep_n - 1u) { /* extremes only -- see console-budget note above */
					cyc = timed_copy(r->a, r->b, words, seedv, iters);
					mix_checksum(r->a, words);
					report_line(r->name, "copy", SWEEP[s].kib, words * 4u * iters, iters, cyc, hz,
						    ceiling_mbps);
				}
			}
		}
	}

	/* ---------------- Phase 1b: Helium 128-bit stores, SRAM0 ------------ */
	printk("\n--- phase 1b: Helium (MVE) 128-bit fill/copy, SRAM0, 1024 KiB ---\n");
	{
		uint32_t words = (1024u * 1024u) / 4u;
		uint32_t iters = 8u;
		uint64_t cyc   = timed_fill_mve(SRAM0_A, words, seed, iters);

		mix_checksum(SRAM0_A, words);
		report_line("SRAM0", "vfill", 1024u, words * 4u * iters, iters, cyc, hz, ceiling_mbps * 4u);
		cyc = timed_copy_mve(SRAM0_B, SRAM0_A, words, iters);
		mix_checksum(SRAM0_B, words);
		report_line("SRAM0", "vcopy", 1024u, words * 4u * iters, iters, cyc, hz, ceiling_mbps * 4u);
	}

	/* ---------------- Phase 2: renderer inner loops, no display -------- */
	printk("\n--- phase 2: renderer inner loops (no display) ---\n");
	init_sprite(seed);
	{
		uint64_t cyc         = timed_sprite_blit(FB_SCRATCH, PANEL_W, SPR_ITERS);
		uint64_t blits_per_s = cyc ? ((uint64_t)SPR_ITERS * hz) / cyc : 0;
		uint64_t px_per_s    = blits_per_s * (SPR_W * SPR_H);

		mix_checksum((const uint32_t *)FB_SCRATCH, (SPR_H * PANEL_W * PANEL_BPP) / 4u);
		printk("sprite 4bpp->rgb565 96x96  iters=%u cycles=%llu  blits/s=%u  px/s=%u\n", SPR_ITERS,
		       (unsigned long long)cyc,
		       (uint32_t)blits_per_s, (uint32_t)px_per_s);
	}

	init_src_row(seed);
	printk("scanline copy (dst=%u px fixed, nearest-neighbour Q16):\n", PANEL_W);
	{
		static const uint32_t src_widths[] = { 720u, 360u, 180u, 90u, 45u };

		for (size_t i = 0; i < ARRAY_SIZE(src_widths); i++) {
			uint32_t sw   = src_widths[i];
			uint64_t cyc  = timed_scanline(sw, PANEL_W, SCANLINE_ITERS);
			uint64_t rows = cyc ? ((uint64_t)SCANLINE_ITERS * hz) / cyc : 0;
			uint64_t bps  = rows * PANEL_W * PANEL_BPP;

			mix_checksum((const uint32_t *)dst_row, (PANEL_W * PANEL_BPP) / 4u);
			printk("  srcW=%3u (%ux stretch)  iters=%u cycles=%9llu  rows/s=%6u  MBps=%u.%02u\n", sw,
			       PANEL_W / sw, SCANLINE_ITERS, (unsigned long long)cyc, (uint32_t)rows, (uint32_t)(bps / 1000000ull),
			       (uint32_t)((bps / 10000ull) % 100ull));
		}
	}

	/* ---------------- Phase 3: with the panel live ---------------------- */
	printk("\n--- phase 3: panel live ---\n");
	alp_display_t *disp = open_display_with_retry();

	if (disp == NULL) {
		printk("RESULT: display did not open after %d attempts -- phase 3 skipped.\n"
		       "The phase 1/2 memory numbers above are unaffected and stand on their own.\n",
		       DISP_OPEN_RETRIES);
		goto done;
	}

	alp_display_caps_t caps;

	if (alp_display_get_caps(disp, &caps) != ALP_OK) {
		printk("RESULT: alp_display_get_caps() failed -- phase 3 skipped.\n");
		alp_display_close(disp);
		goto done;
	}
	if (caps.width != PANEL_W || caps.height != PANEL_H || caps.format != ALP_PIXFMT_RGB565) {
		/* FB_SCRATCH and every byte count below assume exactly this geometry --
		 * a mismatch here would silently under/overrun it, so bail instead. */
		printk("RESULT: panel reports %ux%u fmt=%d, expected %ux%u RGB565 -- phase 3 skipped.\n", caps.width,
		       caps.height, (int)caps.format, PANEL_W, PANEL_H);
		alp_display_close(disp);
		goto done;
	}
	printk("display: opened %ux%u RGB565 (%u B/frame)\n", caps.width, caps.height, PANEL_FRAME_BYTES);

	{
		const struct device *raw_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
		uint32_t             bytes_per_run = PANEL_FRAME_BYTES * FILL_TIMED_ITERS;
		uint64_t             cyc_blanked, cyc_live;
		uint64_t             mbps_blanked, mbps_live;

		/* Blanking ON: CDC200 scanout stopped -- isolates the CPU's write
		 * bandwidth to the framebuffer from any DMA read contention. */
		(void)display_blanking_on(raw_dev); /* optional driver op; CDC200 implements it, ignore the result */
		for (uint32_t n = 0; n < FILL_WARM_ITERS; n++) {
			alp_display_blit(disp, 0, 0, PANEL_W, PANEL_H, FB_SCRATCH);
		}
		uint64_t t0 = k_cycle_get_64();

		for (uint32_t n = 0; n < FILL_TIMED_ITERS; n++) {
			alp_display_blit(disp, 0, 0, PANEL_W, PANEL_H, FB_SCRATCH);
		}
		cyc_blanked = k_cycle_get_64() - t0;
		report_line("panel", "fillOn", 0u, bytes_per_run, FILL_TIMED_ITERS, cyc_blanked, hz, ceiling_mbps);

		/* Blanking OFF: CDC200 scanning continuously (~80 MB/s of reads at the
		 * 40 MHz pixel clock) while the same fill runs -- the contended case. */
		(void)display_blanking_off(raw_dev);
		for (uint32_t n = 0; n < FILL_WARM_ITERS; n++) {
			alp_display_blit(disp, 0, 0, PANEL_W, PANEL_H, FB_SCRATCH);
		}
		t0 = k_cycle_get_64();
		for (uint32_t n = 0; n < FILL_TIMED_ITERS; n++) {
			alp_display_blit(disp, 0, 0, PANEL_W, PANEL_H, FB_SCRATCH);
		}
		cyc_live = k_cycle_get_64() - t0;
		report_line("panel", "fillOff", 0u, bytes_per_run, FILL_TIMED_ITERS, cyc_live, hz, ceiling_mbps);

		/* THE number the whole plan turns on: how much the CDC200's continuous
		 * scanout costs the CPU's own write bandwidth to the same memory. */
		mbps_blanked = cyc_blanked ? ((uint64_t)bytes_per_run * hz) / cyc_blanked / 1000000ull : 0;
		mbps_live    = cyc_live ? ((uint64_t)bytes_per_run * hz) / cyc_live / 1000000ull : 0;
		printk("CONTENTION: blanked=%u MBps  live=%u MBps  delta=%d MBps  (scanout cost)\n",
		       (uint32_t)mbps_blanked, (uint32_t)mbps_live, (int)(mbps_blanked - mbps_live));

		/* Sustained repaint: blanking stays off (normal operating condition) --
		 * fixed frame count, report achieved fps. */
		t0 = k_cycle_get_64();
		for (uint32_t f = 0; f < REPAINT_FRAMES; f++) {
			alp_display_blit(disp, 0, 0, PANEL_W, PANEL_H, FB_SCRATCH);
		}
		uint64_t cyc_repaint  = k_cycle_get_64() - t0;
		uint64_t fps_x100     = cyc_repaint ? ((uint64_t)REPAINT_FRAMES * hz * 100ull) / cyc_repaint : 0;
		uint32_t needed_mbps  = 55u; /* 720x1280 RGB565 @ 30 fps ~= 55.3 MB/s of writes -- see README */

		printk("sustained repaint: frames=%u cycles=%llu  fps=%u.%02u", REPAINT_FRAMES,
		       (unsigned long long)cyc_repaint,
		       (uint32_t)(fps_x100 / 100u), (uint32_t)(fps_x100 % 100u));
		if ((fps_x100 / 100u) < 30u) {
			printk("  ** below 30 fps at %u MBps needed for writes alone **", needed_mbps);
		}
		printk("\n");
	}

	alp_display_close(disp); /* also restores blanking on, via the same backend that turned it off at open() */

done:
	printk("\nchecksum=0x%08x  (every buffer this probe wrote, folded together -- proves none of the\n"
	       "above was optimised away; if this run is ever repeated, a different value here with the\n"
	       "same seed source is expected -- the seed is k_cycle_get_64()-derived, not fixed)\n",
	       g_checksum);
	printk("=== fillrate probe done ===\n");
	return 0;
}
