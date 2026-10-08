/* renderer.c -- renderer image, both A32 cores (T-A6; scene T-A7). Plan:
 * docs/superpowers/plans/2026-09-22-a32-renderer.md sec 4-8.
 *
 * Core 0 (renderer_main): boot self-checks (NEON span fill + z-tested spans
 * vs scalar), render_init, then opens the core-1 gate (below) and runs the
 * mailbox loop: HALT check; take `in`; render_front_begin (scene step);
 * scene_go = seq; sev; build scene part 1 while core 1 builds part 2
 * (tr_scene_build_part(), own DL); wait scene_done; render_front_end
 * (append part 2, background); setup_lo/hi = second half; setup_go = seq;
 * sev; set up the first half of the triangles while core 1 does the
 * second; wait setup_done; render_bin (DL order); band_claim = seq:0;
 * dmb ish; frame_go = seq; sev; claim bands with core 1 (band_claim CAS,
 * LDREX/STREX, only for this seq); wait core1_done; dsb sy; publish out.
 * Every wait on core 1 is bounded (RENDER_SETUP_TIMEOUT scene/setup,
 * RENDER_JOIN_TIMEOUT bands); a timeout makes core 0 finish the frame
 * alone -- the whole scene into its own DL, the other setup half, the
 * bands not yet landed -- and ends dual core for this LAUNCH (pad3[1]).
 * render_setup is the scene (default) or, with RENDER_DL_GOLDEN=1
 * (`make golden`), the committed golden DL. Golden build only, after
 * publish: CRC ONE band of the published FB (rotating over the 40) against
 * the generated per-band golden TR_GOLDEN_BAND_CRC. Scene frames have no
 * host golden: the scene
 * build records per-frame front-end stats instead (pad3 below).
 *
 * Core 1 (renderer_core1): waits for the gate, switches to the renderer
 * table (0x022/0x026/0x027 differ from the stub's), then loops: wait scene_go
 * (WFE, event stream on per the stub), build scene part 2, scene_done = seq;
 * sev; wait setup_go, set up its half, dmb ish,
 * setup_done = seq; sev; wait frame_go == seq, claim + render bands with its
 * OWN z/colour band buffers, dsb sy, core1_done = seq; sev.
 *
 * The gate: core 0 zeroes .bss at entry and core 1 runs concurrently (the
 * stub releases it just before core 0 jumps), so core 1 touches nothing of
 * ours until core 0 writes RENDER_GATE = the launch token. The token is
 * stub_heartbeat0, which the stub's launch() bumps (to a non-zero value) before every
 * jump -- the release self-LAUNCH included, which never visits the park loop -- and
 * which stays frozen while a payload runs: fresh per launch, same on both cores, so
 * a gate value left by an earlier launch never matches, and a zero token cannot
 * meet a gate word that happens to read 0 (RENDER_GATE sits outside .bss, so power-on
 * garbage or an earlier launch's token is all it holds until core 0 writes it).
 *
 * HALT (contract: core 1 returns first): core 1 sees ctrl_cmd == HALT while
 * idle and returns; core 0 sees it, waits (<= RENDER_HALT_WAIT) for
 * stub_core1_state to leave RUNNING, then consumes it (ctrl_cmd = 0) and
 * returns. A fault on either core makes the stub write HALT, which the other
 * core then honours the same way.
 *
 * RENDER_SINGLE_CORE=1 (build define) is the A/B switch: core 1 returns to
 * its park at entry and core 0 renders all 40 bands.
 *
 * An in_fb that is neither TR_FB_A nor TR_FB_B is not written: out_dropped++,
 * still published. out_dropped also counts (triangle, band) pairs lost to a
 * full bin. out_ticks0 = core 0 CNTVCT ticks take -> ready to publish (frame
 * latency); out_ticks1 = core 1 ticks frame_go seen -> done.
 *
 * tr_mbox_t.pad3[] (out-block spare, payload-owned; decode.py mirrors it),
 * CNTVCT ticks at 100 MHz:
 *   pad3[0] RENDER_MARKER: 0x5E4D0003 scene build, 0x5E4D0002 golden build
 *   pad3[1] bit31 ran self-checks, bit0 tr_span_selfcheck, bit1
 *           tr_raster_selfcheck, bit2 dual core active; [15:8] bands core 1
 *           rendered last frame, [23:16] bands core 0, [30:24] core-1
 *           timeouts (scene, setup or band join) since LAUNCH (saturates at
 *           127). The first timeout ends dual core for this LAUNCH: bit2
 *           clears and core 0 renders alone (a late core 1 blocks at its
 *           next wait; HALT still parks it).
 *   pad3[2] setup + bin ticks on core 0 (its half, wait, binning), last frame
 *   pad3[3] core 0 raster ticks (its bands, HUD included), last frame
 *   pad3[4] core 0 copy ticks (band -> FB), last frame
 *   pad3[5] scene: scene ticks (tr_scene_step + build + bg), last frame
 *           golden: last band CRC-32 computed (its band = (checks - 1) % 40)
 *   pad3[6] scene: [15:0] max band bin (of TR_BIN_MAX 1536), [31:16]
 *           front-end dropped tris (tr_dl_dropped), last frame
 *           golden: band CRC checks [15:0] matched, [31:16] mismatched
 *   pad3[7] max out_ticks0 since this LAUNCH
 * out_tris = DL triangles emitted; out_dropped += bin overflow + front-end
 * drops.
 *
 * Profile build (`make prof`, TR_RASTER_PROF): PMCCNTR (A32 core cycles)
 * counters per core from src/render/r3d.h's TR_PROF_* hooks, accumulated
 * since LAUNCH, snapshotted by core 0 after every frame's join into the
 * NC block RENDER_PROF_ADDR (the unused tail of the mailbox page; debug-AP
 * readable, decode.py --prof):
 *   +0x00 RENDER_PROF_MARKER 0x5E4D5052, +0x04 frames rendered,
 *   +0x08 core 0 then core 1: TR_PROF_N x {cycles, px, calls}
 *   (FLAT, GOURAUD, TEX, TRI, BG, SETUP, BIN, NOZ_TEX, NOZ_FILL -- see r3d.h).
 * The hooks cost a few cycles per span; compare stage times against the
 * plain build, not this one.
 */
#include <stdbool.h>
#include <stdint.h>

#include "render.h"

#include "../../src/render/panel_rot.h"
#include "stub_abi.h"
#include "tr_mbox.h"
#include "tr_memmap.h"
#include "../../src/ipc/tr_cam_view.h"
#include "../../src/ipc/tr_hp_dbg.h"
#include "../../src/ipc/tr_pslot.h"
#include "../../src/render/span.h"
#include "../../src/render/r3d.h"
#include "../../src/render/r3d_scene.h"

#if RENDER_DL_GOLDEN
#define RENDER_MARKER 0x5E4D0002u
#else
#define RENDER_MARKER 0x5E4D0003u
#endif
#define RENDER_TTB ((volatile uint32_t *)TR_MEM_RENDER_TTB)
/* The page right above the core-1 stack (tr_memmap.h: stacks end at
 * 0x025E0000, FB B starts at 0x025EA000), WB S=1 in both tables; outside .bss
 * so core 0's zeroing never races core 1's wait. */
#define RENDER_GATE ((volatile uint32_t *)TR_MEM_A32_GATE)
_Static_assert(TR_MEM_A32_STACKS + TR_MEM_A32_STACKS_SIZE == TR_MEM_A32_GATE,
               "the gate sits on the core-1 stack top (start.S)");
#ifndef RENDER_SINGLE_CORE
#define RENDER_SINGLE_CORE 0
#endif
/* One band is ~0.3 ms at 800 MHz; core 1 finishing its last band after core
 * 0 ran out is well under this. */
/* Scene bands average ~0.9 ms on silicon; core 1 finishing its last band
 * after core 0 ran out is well under this. */
#define RENDER_JOIN_TIMEOUT 600000u /* 6 ms of CNTVCT */
/* After publishing, core 0 polls in_seq flat out this long (the M55's
 * out -> in turnaround, flip + vblank wait included, measured <= 2.05 ms),
 * then drops to WFE with the event stream: a held or dead M55 does not get
 * a core spinning on uncached SRAM1 (which the CDC200 is scanning) forever. */
#define RENDER_SPIN_TICKS 300000u /* 3 ms */
/* Both halves of the setup take about as long; core 1 is idle and waiting. */
#define RENDER_SETUP_TIMEOUT 500000u   /* 5 ms */
#define RENDER_HALT_WAIT     50000000u /* 500 ms, inside the stub's 1 s core-1 wait */

/* Section attributes, same encoding as a32/stub/stub.c. */
#define SEC_NC     0x00001C12u /* Normal NC, XN */
#define SEC_NC_X   0x00001C02u /* Normal NC, exec */
#define SEC_WB_S_X 0x00011C0Eu /* Normal WB-WA, S=1, exec */
#define SEC_WB_S   0x00011C1Eu /* Normal WB-WA, S=1, XN */
/* 0x027 through a second-level table: 256 x 4 KiB small pages, 1 KiB,
 * 1 KiB aligned, in the unused MiB-0 scratch (NC, like the L1 tables). */
#define RENDER_L2_027 ((volatile uint32_t *)0x02402000u)
#define L1_PAGE       0x00000001u /* page-table pointer, domain 0 */
#define PTE_NC        0x00000073u /* small page, TEX=001 C=B=0 (Normal NC), AP=11, XN */
#define PTE_NC_X      0x00000072u /* same, XN cleared (bit0): exec, like SEC_NC_X's section */
#define PTE_WB_S_X    0x0000047Eu /* PTE_WB_S with XN cleared: exec, like SEC_WB_S_X's section */
#define PTE_WB_S      0x0000047Fu /* small page, TEX=001 C=B=1 (WB-WA), AP=11, S=1, XN */
/* 0x023 likewise, second table right after: WB pages below the MHU0 window
 * only (scene part 2's DL), the window and everything above fault. */
#define RENDER_L2_023 ((volatile uint32_t *)0x02402400u)
/* 0x025 likewise, third table: the image, stacks, gate and spare as WB-WA S=1 exec pages,
 * then FB B's head (TR_FB_B..0x025FFFFF) Normal NC XN (FB B shares the MiB with the image). */
#define RENDER_L2_025 ((volatile uint32_t *)0x02402800u)
/* 0x024 likewise, fourth table (fix round 10, silicon finding: the camera
 * pool at TR_MEM_CAM_POOL, 0x02480000, is HP-DMA'd GREY8 the video panel
 * reads every frame -- mapped Normal NC like the rest of the old 0x024
 * SECTION, every read was an individual uncached bus transaction, the
 * dominant term in the panel's measured ~117 ns/px, ~50x the ~2 ns/px a
 * cached read + NEON copy-out costs). Mailbox/stub/tables (below
 * TR_MEM_CAM_POOL, same NC-exec permission the section had) stay NC: cross-
 * core signalling words need to stay uncached, and the stub's own launch
 * sequence may still execute out of this MiB. Only the camera pool itself
 * (TR_MEM_CAM_POOL..+TR_MEM_CAM_POOL_SIZE, exactly the MiB's own upper half)
 * becomes WB-WA S=1: draw_video_panel() then does ONE dcache_inval_range()
 * over the frame it is about to read (not once a row), and the actual reads
 * are cached loads, not NC bus transactions. */
#define RENDER_L2_024 ((volatile uint32_t *)0x02402C00u)
_Static_assert(TR_MEM_CAM_POOL == 0x02480000u &&
                   TR_MEM_CAM_POOL + TR_MEM_CAM_POOL_SIZE == 0x02500000u,
               "the camera pool must be exactly the 0x024 MiB's upper half for this page split");
_Static_assert(0x02402C00u + 0x400u <= TR_MEM_MBOX_PAGE_END,
               "renderer L2 tables past the mailbox scratch");

/* Always-on bench block, NC, renderer-owned (mailbox page tail; decode.py
 * --stats): written at LAUNCH, then per frame.
 *   +0x00 RENDER_STATS_MARKER 0x5E4D5354
 *   +0x04 quality: TR_LOD_* bits (r3d_scene.h) the BENCH writes; read every
 *         frame (scene build), bits above 0x07 ignored; reset to 0 = full
 *         detail at every LAUNCH (a release boot runs the default)
 *   +0x08 gap: CNTVCT ticks from publishing out_seq to taking the next
 *         in_seq (the M55's flip + vblank wait + its poll latency), last
 *   +0x0C gap min, +0x10 gap max, +0x14 frames gapped, since LAUNCH
 *   +0x18 this image's end (__bss_end), written at LAUNCH: the HE's perf
 *         panel counts the renderer's real SRAM1 use (TR_RENDER_IMG_END_ADDR)
 *   +0x1C..+0x2C: words 7..11 are feat/edge-aa's / feat/dma-copyout's
 *   +0x30 CNTVCT at renderer_main entry, +0x34 after the self-checks +
 *         render_init, +0x38 when the first out_seq went out (0 before):
 *         LAUNCH -> first frame, which the HE logs (TR_RENDER_T_ADDR) */
#define RENDER_STATS_ADDR   TR_RENDER_STATS_ADDR
#define RENDER_STATS        ((volatile uint32_t *)RENDER_STATS_ADDR)
#define RENDER_STATS_MARKER TR_RENDER_STATS_MARKER
_Static_assert(RENDER_STATS_ADDR + 12u * 4u == TR_RENDER_T_ADDR,
               "tr_mbox.h TR_RENDER_T_ADDR: words 12..14");
_Static_assert(RENDER_STATS_ADDR + 6u * 4u == TR_RENDER_IMG_END_ADDR,
               "tr_mbox.h TR_RENDER_IMG_END_ADDR");
extern char __bss_end[]; /* renderer.ld */

#ifdef TR_RASTER_PROF
#define RENDER_PROF_ADDR   ((volatile uint32_t *)0x02401800u)
#define RENDER_PROF_MARKER 0x5E4D5052u
static tr_prof_t prof[2][TR_PROF_N] __attribute__((aligned(64)));

tr_prof_t *tr_prof_core(void)
{
	uint32_t mpidr;

	__asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
	return prof[mpidr & 1u];
}

uint32_t tr_prof_now(void)
{
	uint32_t c;

	__asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(c));
	return c;
}

/* PMCR.E (enable) | PMCR.C (reset the cycle counter); PMCNTENSET.C. */
static void prof_enable(void)
{
	__asm__ volatile("mcr p15, 0, %0, c9, c12, 0\n\t"
	                 "mcr p15, 0, %1, c9, c12, 1\n\tisb" ::"r"(5u),
	                 "r"(0x80000000u));
}

static void prof_publish(void)
{
	static uint32_t    frames;
	volatile uint32_t *d = RENDER_PROF_ADDR;
	const uint32_t    *p = (const uint32_t *)prof;

	d[0] = RENDER_PROF_MARKER;
	d[1] = ++frames;
	for (uint32_t i = 0; i < sizeof(prof) / 4u; i++)
		d[2 + i] = p[i];
}
#else
static void prof_enable(void)
{
}
static void prof_publish(void)
{
}
#endif

void render_build_table(void);
void renderer_main(volatile tr_mbox_t *m);
void renderer_core1(volatile tr_mbox_t *m);

/* Everything not listed faults. Both cores run on this table (core 1
 * switches after the gate): 0x022 and 0x026 differ from the stub table.
 *   0x020-0x021 FB A slot      Normal NC, XN sections (the FB is only written);
 *               the slot ends 0x021F4000, the rest of 0x021 is free
 *   0x022       setup + bands + zone textures + zone indices  Normal WB-WA S=1, XN (A32-only, cached RMW)
 *   0x023       scene part-2 DL 0x02300000..0x0237FFFF as WB-WA S=1 4 KiB
 *               pages; 0x02380000.. (the TF-A MHU0 window) faults
 *   0x024       mailbox, stub, tables -- NC, VA == PA, exec (contract), through
 *               4 KiB pages below TR_MEM_A32_DL (0x02424000); from there the DL,
 *               bins and the camera pool (TR_MEM_CAM_POOL..+SIZE, fix round 10)
 *               Normal WB-WA S=1, XN
 *   0x025       4 KiB pages: image + .bss (to 0x025C0000), stacks, gate, spare --
 *               WB-WA S=1, exec; from TR_FB_B (0x025EA000) FB B's head, Normal NC, XN
 *   0x026       FB B (middle MiB)  Normal NC, XN
 *   0x027       FB B tail 0x02700000..0x027DDFFF as NC 4 KiB pages; the rest
 *               of the MiB (TF-A RW 0x027DE000..) faults */
void render_build_table(void)
{
	volatile uint32_t *t = RENDER_TTB;

	for (uint32_t i = 0; i < 4096u; i++)
		t[i] = 0;
	t[0x020] = 0x02000000u | SEC_NC;
	t[0x021] = 0x02100000u | SEC_NC;
	t[0x022] = 0x02200000u | SEC_WB_S;
	for (uint32_t p = 0; p < 256u; p++) {
		uint32_t va = 0x02400000u + (p << 12);

		RENDER_L2_024[p] = va | (va < TR_MEM_A32_DL ? PTE_NC_X : PTE_WB_S);
	}
	t[0x024] = 0x02402C00u | L1_PAGE;
	for (uint32_t p = 0; p < 256u; p++) {
		uint32_t va = 0x02500000u + (p << 12);

		RENDER_L2_025[p] = va < TR_FB_B ? (va | PTE_WB_S_X) : (va | PTE_NC);
	}
	t[0x025] = 0x02402800u | L1_PAGE;
	t[0x026] = 0x02600000u | SEC_NC;
	for (uint32_t p = 0; p < 256u; p++) {
		uint32_t va = 0x02700000u + (p << 12);

		RENDER_L2_027[p] = va < TR_FB_B + TR_FB_SLOT_SIZE ? (va | PTE_NC) : 0u;
	}
	t[0x027] = 0x02402000u | L1_PAGE;
	for (uint32_t p = 0; p < 256u; p++) {
		uint32_t va = 0x02300000u + (p << 12);

		/* fix round 12 (review, major): TR_MEM_ARING..TR_MHU0_WINDOW_LO
		 * (one page, 0x0237F000..0x0237FFFF) holds the sound ring, the
		 * pose slot, hp_dbg and tr_cam_view -- all HP-written, cross-
		 * core, seqlock-protected structs the A32 only ever reads via a
		 * CPU store on the HP's side (not DMA, unlike CAM_POOL). Mapping
		 * it WB-WA S=1 like the scene DL pages around it let the A32
		 * accept a STALE but internally consistent cached copy: the
		 * seqlock's own re-check compares against a cached s->seq that
		 * never gets re-fetched, so a torn-looking read never fires even
		 * when the real, current SRAM0 word has moved on. It "worked"
		 * only because the renderer's own DL traffic kept evicting
		 * those lines often enough to refresh them by accident, not
		 * because reads were actually coherent. Normal NC here, same as
		 * CAM_POOL's own reasoning one MiB up, so this address range
		 * is exempted from the WB-WA span below it. */
		bool nc_page = va >= TR_MEM_ARING && va < TR_MHU0_WINDOW_LO;

		RENDER_L2_023[p] = va < TR_MHU0_WINDOW_LO ? (va | (nc_page ? PTE_NC : PTE_WB_S)) : 0u;
	}
	t[0x023] = 0x02402400u | L1_PAGE;
}
_Static_assert(
    TR_MEM_ARING == 0x0237F000u && TR_MHU0_WINDOW_LO == 0x02380000u &&
        TR_MHU0_WINDOW_LO - TR_MEM_ARING == 0x1000u,
    "the shared cross-core metadata page (sound ring, pslot, hp_dbg, cam_view) must be exactly "
    "one 4 KiB page ending right at the MHU0 window, so the render_build_table loop above maps "
    "the whole thing -- and only that -- Normal NC");
_Static_assert(TR_MEM_PSLOT >= TR_MEM_ARING && TR_MEM_HP_DBG >= TR_MEM_ARING &&
                   TR_MEM_CAM_VIEW >= TR_MEM_ARING &&
                   TR_MEM_CAM_VIEW + sizeof(tr_cam_view_t) <= TR_MHU0_WINDOW_LO,
               "tr_pslot_t/hp_dbg_t/tr_cam_view_t must all sit inside the one NC page above");
_Static_assert(TR_FB_A == 0x02000000u && TR_FB_A + TR_FB_SIZE <= 0x02200000u,
               "FB A must be SRAM0 MiB 0-1");
_Static_assert(TR_FB_A + TR_FB_SLOT_SIZE <= 0x02200000u, "FB A's slot must end in SRAM0 MiB 1");
_Static_assert((TR_MEM_A32_DL & 0xFFFu) == 0 && TR_MEM_A32_DL >= STUB_STACK1_TOP &&
                   TR_MEM_A32_DL < TR_MEM_CAM_POOL,
               "the DL starts on a page after the stub stacks, in the 0x024 MiB's WB-page span");
_Static_assert(TR_MEM_A32_GATE + 0x1000u <= TR_FB_B && (TR_FB_B & 0xFFFu) == 0u,
               "FB B must be page aligned and start above the gate page");
_Static_assert(TR_FB_B == 0x025EA000u && TR_FB_B + TR_FB_SLOT_SIZE <= TR_MEM_TFA_RW,
               "FB B must be 0x025EA000 and its slot must end at or below TF-A RW");
_Static_assert(TR_FB_B > TR_MEM_A32_IMG_END && TR_FB_B < 0x02600000u,
               "FB B's head must be in the 0x025 MiB (the L2_025 split), above the image cap");
_Static_assert(TR_FB_B + TR_FB_SLOT_SIZE > 0x02700000u && TR_FB_B + TR_FB_SLOT_SIZE <= 0x027E0000u,
               "FB B's slot must reach into 0x027 (the L2_027 map) and end inside it");

/* Core 1's switch to the renderer table: the start.S sequence core 0 ran. */
static void core1_use_render_table(void)
{
	__asm__ volatile(
	    "dsb sy\n\t"
	    "mcr p15, 0, %0, c2, c0, 0\n\t" /* TTBR0 (low bits 0: NC walks, like the stub) */
	    "isb\n\t"
	    "mcr p15, 0, %1, c8, c7, 0\n\t" /* TLBIALL */
	    "mcr p15, 0, %1, c7, c5, 6\n\t" /* BPIALL */
	    "dsb sy\n\tisb" ::"r"(RENDER_TTB),
	    "r"(0u)
	    : "memory");
}

static void barrier(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}
static void dmb_ish(void)
{
	__asm__ volatile("dmb ish" ::: "memory");
}
static void sev(void)
{
	__asm__ volatile("sev" ::: "memory");
}
static void wfe(void)
{
	__asm__ volatile("wfe" ::: "memory");
} /* event stream: <= ~0.66 ms */

static inline uint32_t cntvct_lo(void)
{
	uint32_t lo, hi;

	__asm__ volatile("isb\n\tmrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi)::"memory");
	(void)hi;
	return lo;
}

/* Cross-core words, .bss (WB S=1: exclusives need Shareable Normal memory),
 * one cache line each. */
static volatile uint32_t frame_go
    __attribute__((aligned(64))); /* seq of the frame to band, core 0 writes */
/* Band claims, LDREX/STREX: [31:8] the frame's seq (low 24 bits), [7:0]
 * the next unclaimed band. A core claims only for the seq it is rendering,
 * so a late core 1 can never take a band of a newer frame. */
static volatile uint32_t band_claim __attribute__((aligned(64)));
static volatile uint32_t core1_done
    __attribute__((aligned(64)));     /* seq core 1 finished, core 1 writes */
static volatile uint32_t core1_ticks; /* its busy ticks for core1_done's frame */
static volatile uint32_t scene_go
    __attribute__((aligned(64))); /* seq whose scene part 2 core 1 builds */
static volatile uint32_t scene_done __attribute__((aligned(64))); /* seq core 1 finished building */
static volatile uint32_t setup_go
    __attribute__((aligned(64))); /* seq whose setup half core 1 does */
static volatile uint32_t setup_done
    __attribute__((aligned(64)));   /* seq core 1 finished setting up */
static uint32_t setup_lo, setup_hi; /* core 1's half, written before setup_go */
/* fix round 10: claimable work is every 3D band (0..TR_BANDS-1) AND every
 * video band (TR_BANDS..TR_TOTAL_BANDS-1, render.h's TR_VIDEO_BANDS) through
 * the SAME band_claim counter -- the video panel used to be one single-
 * threaded core-0-only call after every 3D band landed (35.9 ms/frame
 * silicon regression, all on core 0); folding it into this same claim
 * scheme instead of a fixed half/half split means whichever core is free
 * claims the next unit, 3D or video, no new synchronisation needed. */
#define TR_TOTAL_BANDS (TR_BANDS + TR_VIDEO_BANDS)
static volatile uint32_t band_seq[TR_TOTAL_BANDS]
    __attribute__((aligned(64))); /* seq each band last landed for */
static uint16_t *frame_fb;        /* written before frame_go */

/* Claim + render bands (3D, then video) until none are left; each band's
 * pixels are out (dsb) before band_seq says so. */
static void band_loop(uint32_t core, uint32_t seq, uint16_t *fb)
{
	uint32_t v = band_claim;

	for (;;) {
		if ((v >> 8) != (seq & 0xFFFFFFu) || (v & 0xFFu) >= TR_TOTAL_BANDS) return;
		if (!__atomic_compare_exchange_n(
		        &band_claim, &v, v + 1u, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
			continue; /* v reloaded */

		uint32_t b = v & 0xFFu;

		if (b < TR_BANDS)
			render_band(core, (int)b, fb);
		else
			render_video_band(core, (int)(b - TR_BANDS), fb);
		barrier();
		band_seq[b] = seq;
		v           = band_claim;
	}
}

void renderer_core1(volatile tr_mbox_t *m)
{
	uint32_t token = m->stub_heartbeat0;

	if (RENDER_SINGLE_CORE) return;
	prof_enable();
	while (*RENDER_GATE != token) {
		if (m->ctrl_cmd == STUB_CMD_HALT) return;
		wfe();
	}
	dmb_ish();
	core1_use_render_table(); /* built by core 0 before the gate */
	for (uint32_t seen = 0;;) {
		uint32_t seq = scene_go;

		if (seq == seen) {
			if (m->ctrl_cmd == STUB_CMD_HALT) return; /* core 0 consumes it once we are parked */
			wfe();
			continue;
		}
		dmb_ish(); /* scene state + input copy before scene_go */
		uint32_t t0 = cntvct_lo();

		render_core_stats[1] = (render_core_stats_t){ 0 };
		render_front_part(2);
		dmb_ish(); /* part-2 DL before the flag */
		scene_done = seq;
		barrier();
		sev();
		while (setup_go != seq) {
			if (m->ctrl_cmd == STUB_CMD_HALT) return;
			wfe();
		}
		dmb_ish(); /* DL, setup_lo/hi before setup_go */
		render_setup_part(setup_lo, setup_hi);
		dmb_ish(); /* records (WB S=1) before the flag */
		setup_done = seq;
		barrier();
		sev();
		while (frame_go != seq) {
			if (m->ctrl_cmd == STUB_CMD_HALT) return;
			wfe();
		}
		dmb_ish(); /* frame_fb, bins, band_claim before frame_go */
		band_loop(1, seq, frame_fb);
		core1_ticks = cntvct_lo() - t0;
		barrier(); /* this core's NC FB writes land before the flag */
		core1_done = seq;
		barrier();
		sev();
		seen = seq;
	}
}

/*
 * Core 0's joins. On a timeout the caller finishes the frame ALONE and turns
 * dual-core off for the rest of this LAUNCH (it never advances scene_go /
 * setup_go / frame_go again, so a late core 1 blocks at its next wait, where
 * it still honours HALT). Core 0 never redoes core 1's step into core 1's
 * buffer: a late core 1 may still be writing it (DL1->n, setup records,
 * band pixels -- the latter two only with the same bits core 0 writes).
 */
static int join_scene(uint32_t seq)
{
	uint32_t t0 = cntvct_lo();

	while (scene_done != seq) {
		if (cntvct_lo() - t0 > RENDER_SETUP_TIMEOUT) return 0;
		wfe();
	}
	dmb_ish();
	return 1;
}

/* On timeout the caller sets up core 1's half itself: same DL, so the same
 * bits as whatever core 1 may still be writing there. */
static int join_setup(uint32_t seq)
{
	uint32_t t0 = cntvct_lo();

	while (setup_done != seq) {
		if (cntvct_lo() - t0 > RENDER_SETUP_TIMEOUT) {
			return 0;
		}
		wfe();
	}
	dmb_ish();
	return 1;
}

/* Core 0's join: false on timeout, after rendering what core 1 left. */
static int join_core1(uint32_t seq)
{
	uint32_t t0 = cntvct_lo();

	while (core1_done != seq) {
		if (cntvct_lo() - t0 > RENDER_JOIN_TIMEOUT) {
			dmb_ish();
			for (uint32_t b = 0; b < TR_TOTAL_BANDS; b++) {
				if (band_seq[b] == seq) continue;
				if (b < TR_BANDS)
					render_band(0, (int)b, frame_fb);
				else
					render_video_band(0, (int)(b - TR_BANDS), frame_fb);
			}
			return 0;
		}
		wfe();
	}
	dmb_ish();
	return 1;
}

/* The mailbox ABI is wrong (a stub of another TR_MBOX_VERSION, or a frame whose
 * rotation this renderer cannot produce): record what in pad3[7] -- 0xAB1D in
 * the top half, 1 version / 2 rotation | rotation << 8 below -- and fault.
 * The stub records the fault (UNDEF) and parks; there is no fallback drawing
 * of a frame the HE did not ask for. */
static void __attribute__((noreturn)) abi_fault(volatile tr_mbox_t *m, uint32_t what)
{
	m->pad3[7] = 0xAB1D0000u | what;
	barrier();
	__builtin_trap();
}

void renderer_main(volatile tr_mbox_t *m)
{
	uint32_t last = m->out_seq; /* a frame published before LAUNCH is still owed */
	uint32_t tmax = 0, crc_ok = 0, crc_bad = 0, drawn_n = 0, timeouts = 0;
	uint32_t checks, t_pub = 0;
	int      t_pub_valid = 0;
	/* fix round 10 (silicon finding): nothing on the boot path -- not the
	 * stub, not any earlier renderer LAUNCH -- ever zeroed out_dropped, so a
	 * cold mailbox page carries whatever garbage word was last at 0x02401154
	 * into the very first published frame. Every other per-LAUNCH counter
	 * here (RENDER_STATS, pad3[]) is explicitly reset a few lines below;
	 * out_dropped just never was. Zero it, not read it, at LAUNCH. */
	m->out_dropped      = 0;
	tr_frame_out_t o    = { .frames = m->out_frames, .dropped = 0 };
	int            dual = !RENDER_SINGLE_CORE && m->stub_core1_state == STUB_CORE1_RUNNING;

	RENDER_STATS[12] = cntvct_lo();
	RENDER_STATS[13] = 0;
	RENDER_STATS[14] = 0;
	m->pad3[0]       = RENDER_MARKER;
	for (uint32_t i = 1; i < 8u; i++)
		m->pad3[i] = 0;
	checks     = 0x80000000u | (tr_span_selfcheck() ? 1u : 0u) | (tr_raster_selfcheck() ? 2u : 0u) |
	             (dual ? 4u : 0u);
	m->pad3[1] = checks;
	if (m->version != TR_MBOX_VERSION) {
		abi_fault(m, 1u);
	}
	RENDER_STATS[1] = 0;
	RENDER_STATS[2] = 0;
	RENDER_STATS[3] = 0xFFFFFFFFu;
	RENDER_STATS[4] = 0;
	RENDER_STATS[5] = 0;
	RENDER_STATS[6] = (uint32_t)(uintptr_t)__bss_end;
	RENDER_STATS[0] = RENDER_STATS_MARKER;
	prof_enable();
	render_init();
	RENDER_STATS[13] = cntvct_lo();
	dmb_ish();
	*RENDER_GATE = m->stub_heartbeat0; /* core 1 may touch .bss from here */
	barrier();
	sev();

	for (;;) {
		tr_frame_in_t in;
		uint32_t      fb, seq;

		if (m->ctrl_cmd == STUB_CMD_HALT) {
			uint32_t t0 = cntvct_lo();

			sev(); /* core 1 returns first (contract) */
			while (m->stub_core1_state == STUB_CORE1_RUNNING && cntvct_lo() - t0 < RENDER_HALT_WAIT)
				wfe();
			m->ctrl_cmd = STUB_CMD_NONE; /* consumer clears */
			barrier();
			return;
		}
		m->out_heartbeat = ++o.heartbeat;
		/* Within RENDER_SPIN_TICKS of a publish: spin (the event stream
		 * would add up to ~0.66 ms before every frame -- the M55 cannot SEV
		 * us); the heartbeat, HALT and the full take run every ~2000 polls
		 * of in_seq. Later (held or dead M55): WFE, event-stream paced. */
		if (t_pub_valid && cntvct_lo() - t_pub < RENDER_SPIN_TICKS) {
			for (uint32_t i = 0; i < 2000u && m->in_seq == last; i++) {
				__asm__ volatile("" ::: "memory");
			}
		} else if (m->in_seq == last) {
			wfe();
		}
		if (!tr_mbox_take_in(m, last, &in, &fb, &seq, barrier)) {
			continue;
		}
		/* Only a frame that WILL be drawn is held to the rotation ABI: a frame with no
		 * valid framebuffer is dropped below (out_dropped++), whatever its other
		 * fields hold. */
		int drawn = fb == TR_FB_A || fb == TR_FB_B;

		if (tr_rot_refuse(drawn, in.rotation)) {
			abi_fault(m, 2u | ((uint32_t)in.rotation & 0xFFu) << 8); /* low byte only: 270 -> 14 */
		}
		if (t_pub_valid) {
			uint32_t gap = cntvct_lo() - t_pub;

			RENDER_STATS[2] = gap;
			RENDER_STATS[3] = gap < RENDER_STATS[3] ? gap : RENDER_STATS[3];
			RENDER_STATS[4] = gap > RENDER_STATS[4] ? gap : RENDER_STATS[4];
			RENDER_STATS[5] = RENDER_STATS[5] + 1u;
		}
		render_set_quality(
		    (uint8_t)(RENDER_STATS[1] & (TR_LOD_NO_BACK_RANK | TR_LOD_NEAR | TR_LOD_STILL)));

		uint32_t t = 0;

		o.ticks1 = 0;
		if (drawn) {
			uint32_t t0 = cntvct_lo();

			render_core_stats[0] = (render_core_stats_t){ 0 };

			render_front_begin(&in);
			if (dual) {
				dmb_ish(); /* scene step + input copy before scene_go */
				scene_go = seq;
				barrier();
				sev();
			}
			int part2 = dual;

			render_front_part(dual ? 1 : 0);
			if (dual && !join_scene(seq)) {
				/* Core 1 is late: rebuild the whole scene into core 0's
				 * own DL (DL1 may still be written) and go single-core. */
				timeouts += timeouts < 127u;
				dual = part2 = 0;
				checks &= ~4u;
				render_front_part(0);
			}

			uint32_t n = render_front_end(part2), h = dual ? n / 2u : n, tb = cntvct_lo();

			if (dual) {
				setup_lo = h;
				setup_hi = n;
				dmb_ish(); /* DL + split before setup_go */
				setup_go = seq;
				barrier();
				sev();
			}
			render_setup_part(0, h);
			if (dual && !join_setup(seq)) {
				timeouts += timeouts < 127u;
				dual = 0;
				checks &= ~4u;
				render_setup_part(h, n);
			}
			render_bin();
			render_stats.bin = cntvct_lo() - tb;
			frame_fb         = (uint16_t *)fb;
			band_claim       = seq << 8; /* this frame's generation, band 0 */
			if (dual) {
				dmb_ish(); /* bins, frame_fb, band_claim before frame_go */
				frame_go = seq;
				barrier();
				sev();
			}
			band_loop(0, seq, (uint16_t *)fb);
			if (dual) {
				if (!join_core1(seq)) {
					timeouts += timeouts < 127u;
					dual = 0;
					checks &= ~4u;
				} else {
					o.ticks1 = core1_ticks;
				}
			}
			/* fix round 10: every band is drawn now, 3D AND video (either
			 * core, self-balanced by band_loop's own claiming) -- only the
			 * skeleton/lamps/label overlay is left, still a single small
			 * pass by core 0 (it was never the measured cost). */
			render_video_overlay((uint16_t *)fb);
			{
				/* Worst core's own video-band ticks this frame (the two
				 * run concurrently -- summing them would double-count
				 * overlapped work, not report the ≤ 2 ms/core target this
				 * fix's ask is stated against) plus the overlay's own
				 * ticks (core 0, serial, after the join -- always adds to
				 * core 0's own wall time). */
				static uint32_t panel_us_max;
				uint32_t        v0       = render_core_stats[0].video_ticks;
				uint32_t        v1       = dual ? render_core_stats[1].video_ticks : 0u;
				uint32_t        panel_us = ((v0 > v1 ? v0 : v1) + render_video_panel_ticks) / 100u;

				panel_us_max = panel_us > panel_us_max ? panel_us : panel_us_max;
				RENDER_STATS[15] =
				    panel_us; /* this frame (fix round 8 item 3, formula fix round 10) */
				RENDER_STATS[16] = panel_us_max; /* max since this LAUNCH */
			}
			barrier(); /* pixels reach SRAM0 before out_seq says so (plan sec 3) */
			t      = cntvct_lo() - t0;
			tmax   = t > tmax ? t : tmax;
			o.tris = render_stats.tris;
			o.dropped += render_stats.dropped + render_stats.dl_dropped;
			m->pad3[1] = checks | render_core_stats[0].bands << 16 |
			             (dual ? render_core_stats[1].bands : 0u) << 8 | timeouts << 24;
			m->pad3[2] = render_stats.bin;
			m->pad3[3] = render_core_stats[0].raster;
			m->pad3[4] = render_core_stats[0].copy;
			m->pad3[7] = tmax;
			if (!RENDER_DL_GOLDEN) {
				m->pad3[5] = render_stats.scene;
				m->pad3[6] = render_stats.dl_dropped << 16 | (render_stats.max_bin & 0xFFFFu);
			}
		} else {
			o.dropped++;
		}
		if (drawn) prof_publish();
		o.fb     = fb;
		o.ticks0 = t;
		o.frames++;
		tr_mbox_publish_out(m, &o, seq, barrier);
		t_pub = cntvct_lo();
		if (!t_pub_valid) RENDER_STATS[14] = t_pub;
		t_pub_valid = 1;
		last        = seq;

		/* After publish: the M55 only scans this FB out, nobody writes it.
		 * One band per frame (~46 KB NC read), rotating. */
		if (RENDER_DL_GOLDEN && drawn) {
			int      b = (int)(drawn_n++ % TR_BANDS);
			uint32_t c = render_band_crc((const uint16_t *)fb, b);

			if (c == render_golden_band_crc[b])
				crc_ok++;
			else
				crc_bad++;
			m->pad3[5] = c;
			m->pad3[6] = (crc_bad << 16) | (crc_ok & 0xFFFFu);
		}
	}
}
