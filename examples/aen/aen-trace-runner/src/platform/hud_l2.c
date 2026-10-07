/*
 * src/platform/hud_l2.c -- the HUD on CDC200 layer 2 (P9, TR_RENDER=A32).
 *
 * The HE owns layer 2 outright: a 720 x 352 ARGB4444 window over the top of
 * the panel, scanned from its own buffer TR_HUD_FB (tr_mbox.h, SRAM0
 * 0x02382000..0x023FDBFF), blended per pixel over layer 1 (the A32's 3D
 * frame). Zero A32 cost: the renderer skips its own sprite HUD on frames
 * flagged TR_FLAG_HUD_L2, which main.c sets only once tr_hud_l2_open() says
 * the layer is up. Everything drawn is src/hud/hud.c (host-tested); this
 * file is only the registers, the counters and the call.
 *
 * Rotated panel (alp_display_caps_t.rotation 90 or 270, the RVT121): the layer
 * is a 352 x 720 window, TR_HUD_H landscape columns wide at the layer-1
 * window's edge the portrait top lands on (render/panel_rot.h) -- right for 90,
 * left for 270 -- same bytes, same buffer, hud.c writes it rotated. The window
 * is computed here at open from the rotation and the layer-1 window.
 *
 * Register programming follows the Alif DFP driver's own sequence,
 * cdc_set_layer_cfg() in alif-dfp drivers/source/cdc.c (register map
 * drivers/include/cdc.h + soc.h CDC_CDC_LAYER_CFG_Type; offsets and bit
 * names here from the in-tree Zephyr driver's display_cdc200.h):
 *   1. L2 REL_CTRL = SH_MASK (bit 2): mask the global shadow reload for this
 *      layer while it is half-programmed. The Zephyr CDC200 ISR writes
 *      SRCTRL = IMR on EVERY line IRQ (display_cdc200.c cdc200_isr), so an
 *      unmasked half-written window could go live mid-sequence.
 *   2. WIN_HPOS/VPOS: stop << 16 | start, in total-timing coordinates:
 *      start = accumulated sync + back porch + 1 + x0 (BP_CFG holds the
 *      accumulated value minus 1: cdc_set_cfg()), stop = start + w - 1 --
 *      the same arithmetic as display_cdc200.c cdc200_layer_window_set().
 *   3. PIX_FORMAT = 7 (CDC_PIXEL_FORMAT_ARGB4444).
 *   4. CONST_ALPHA = 255; BLEND_CFG = F1 6 << 8 | F2 7 (pixel alpha x
 *      constant alpha, and its inverse): c' = a*c_L2 + (1 - a)*c_below.
 *   5. CFB_ADDR = TR_HUD_FB; CFB_LENGTH = pitch << 16 | (line bytes + 7)
 *      (BUS_WIDTH 7: cdc.h); CFB_LINES = 352.
 *   6. CTRL = LAYER_EN (bit 0) -- default-colour blend and CLUT off.
 *   7. REL_CTRL |= SH_VBLANK (bit 1): the whole set lands in the next
 *      vertical blanking. SH_MASK stays set: layer 2 never changes again,
 *      and the driver's per-frame global reload cannot touch it.
 * Before step 7 the buffer is cleared to 0 (fully transparent), so the
 * layer's first visible frame is see-through, never stale SRAM. The layer
 * counts as up only once the reload landed (SH_VBLANK self-cleared -- seen
 * on 2026W36-0009, docs/2026-09-22-measurements.md) and CTRL / CFB_ADDR read back
 * as written; otherwise it is switched off again and the A32 keeps its
 * sprite HUD.
 *
 * Single buffered, deliberately: the HUD is repainted right after frame N+1
 * is published, while the glass still shows frame N, so score and popups
 * lead the 3D picture by one refresh (25 ms), and a tile copied across the
 * scan line tears for one refresh (the 16-row strip keeps it to that tile).
 * A second 506,880 B buffer has no room in SRAM0 (tr_memmap.h).
 *
 * No cache maintenance: SRAM0 (0x02xxxxxx) has no MPU region on the HE and
 * falls to the ARMv8-M default map's Code region, Normal Write-Through, so
 * the CDC200 sees every store even when a previous image left CCR.DC on.
 */
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>

#include <display_cdc200.h>

#include "../hud/hud.h"
#include "../ipc/tr_aring.h"
#include "../ipc/tr_mbox.h"
#include "../ipc/tr_memmap.h"
#include "../render/panel_rot.h"
#include "display.h"
#include "hud_l2.h"
#include "rail5v_power.h"

#define CDC_REGS DT_REG_ADDR(DT_NODELABEL(cdc200))
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(cdc200)) == 0x49031000u, "CDC200 base (soc.h CDC_BASE)");
/* The layer-1 window the HUD sits on (the whole panel unless a shield-fit overlay
 * narrows it), in panel coordinates. */
#define L1_NODE DT_NODELABEL(cdc200)
#define L1_X0   DT_PROP_OR(L1_NODE, win_x0_l1, 0)
#define L1_Y0   DT_PROP_OR(L1_NODE, win_y0_l1, 0)
#define L1_W    (DT_PROP_OR(L1_NODE, win_x1_l1, DT_PROP(L1_NODE, width)) - L1_X0)
#define L1_H    (DT_PROP_OR(L1_NODE, win_y1_l1, DT_PROP(L1_NODE, height)) - L1_Y0)
BUILD_ASSERT(TR_HUD_FB_SIZE == TR_HUD_W * TR_HUD_H * 2u, "tr_mbox.h TR_HUD_FB_SIZE");
BUILD_ASSERT(TR_HUD_FB % 64u == 0u,
             "CDC200 fetch address alignment (bus width 8 B; 64 B for burst)");
/* The layer never touches what the A32 renders into or TF-A's window. */
BUILD_ASSERT(TR_HUD_FB >= TR_MHU0_WINDOW_HI && TR_HUD_FB >= TR_FB_A + TR_FB_SIZE,
             "HUD buffer placement");

/* Repaint cap per presented frame (tr_hud_t.budget). Host-counted M55
 * instructions (qemu-arm -cpu cortex-m55, tools/hud_preview.c's layouts):
 * ~9.4 per repainted px on average, so 110,000 px is ~1.0 M instructions,
 * ~6-10 ms at 160 MHz -- well inside the ~20 ms the HE waits on the A32
 * every frame. Only a screen change hits it (spread over 3 frames); a
 * score + popup frame is ~101,000 px. */
#define HUD_PX_BUDGET 110000u

static uint16_t *const g_fb = (uint16_t *)TR_HUD_FB;
static bool            g_up;
static tr_hud_t        g_hud;
static tr_hud_view_t   g_view;
static tr_perf_t       g_perf;

/* Bench-readable (nm): HUD cost per presented frame, HE cycles
 * (k_cycle_get_32, the 160 MHz core clock), and what it repainted. */
volatile uint32_t tr_hud_cyc_last, tr_hud_cyc_max, tr_hud_updates, tr_hud_repaints;
volatile uint64_t tr_hud_cyc_total, tr_hud_px_total;
/* Layer 2 readback after open: CTRL, WIN_HPOS, WIN_VPOS, CFB_ADDR, REL_CTRL. */
volatile uint32_t tr_hud_l2_regs[5];
/* The CDC200 driver's error IRQ counts (display_cdc200.c cdc200_isr), copied
 * every frame: the second layer's fetch shares SRAM0 with the A32, and a
 * starved layer FIFO shows here, not on any host test. Bench-only. */
volatile uint32_t tr_cdc_fifo_underruns, tr_cdc_bus_errs;

/* Linker symbols (Zephyr): ITCM image bytes (= zephyr.bin) and DTCM static RAM. */
extern char _flash_used[], _image_ram_size[];

static inline uint32_t rd(uint32_t off)
{
	return sys_read32(CDC_REGS + off);
}

static inline void wr(uint32_t off, uint32_t v)
{
	sys_write32(v, CDC_REGS + off);
}

bool tr_hud_l2_open(void)
{
	int rot = tr_display_rotation();

	uint32_t lcnt = rd(CDC_LCNT), bp = rd(CDC_BP_CFG);
	/* accumulated HSYNC + HBP (minus 1) in [31:16], VSYNC + VBP in [15:0] */
	uint32_t ahbp = (bp >> 16) & 0xFFFFu, avbp = bp & 0xFFFFu;
	uint32_t hs = ahbp + 1u, vs = avbp + 1u;
	/* The window and pitch of the layer: 720 x 352, or turned 352 x 720. */
	uint32_t win_w = rot ? TR_ROT_HUD_W : TR_HUD_W, win_h = rot ? TR_ROT_PORTRAIT_W : TR_HUD_H;
	uint32_t pitch = win_w * 2u;

	if (!tr_rot_valid(rot) || win_w > L1_W || win_h > L1_H || (rot == 0 && win_w != L1_W)) {
		printk("hud     : %ux%u layer 2 (rotation %d) does not fit the %ux%u scan-out window -- "
		       "A32 keeps the HUD\n",
		       (unsigned)win_w,
		       (unsigned)win_h,
		       rot,
		       (unsigned)L1_W,
		       (unsigned)L1_H);
		return false;
	}
	if (lcnt < 2u) {
		printk("hud     : CDC200 reports %u layer(s) -- no layer 2, A32 keeps the HUD\n", lcnt);
		return false;
	}
	memset(g_fb, 0, TR_HUD_FB_SIZE); /* transparent before the layer is ever shown */
	tr_hud_init(&g_hud);
	g_hud.rot    = rot;
	g_hud.budget = HUD_PX_BUDGET;
	memset(&g_view, 0, sizeof(g_view));

	wr(CDC_L2_REL_CTRL, CDC_LN_REL_CTRL_SH_MASK);
	hs += L1_X0 + (rot == 90 ? L1_W - win_w : 0u);
	vs += L1_Y0;
	wr(CDC_L2_WIN_HPOS, (hs + win_w - 1u) << CDC_LN_WIN_HPOS_STOP_POS_SHIFT | hs);
	wr(CDC_L2_WIN_VPOS, (vs + win_h - 1u) << CDC_LN_WIN_VPOS_STOP_POS_SHIFT | vs);
	wr(CDC_L2_PIX_FORMAT, CDC_PIXEL_FORMAT_ARGB4444);
	wr(CDC_L2_CONST_ALPHA, 255u);
	wr(CDC_L2_BLEND_CFG,
	   CDC_BLEND_PIXEL_ALPHA_X_CONST_ALPHA << CDC_LN_BLEND_CFG_F1_SEL_SHIFT |
	       CDC_BLEND_PIXEL_ALPHA_X_CONST_ALPHA_INV);
	wr(CDC_L2_CFB_ADDR, TR_HUD_FB);
	wr(CDC_L2_CFB_LENGTH, pitch << CDC_LN_CFB_LENGTH_PITCH_SHIFT | (pitch + BUS_WIDTH));
	wr(CDC_L2_CFB_LINES, win_h);
	wr(CDC_L2_CTRL, CDC_LN_CTRL_LAYER_EN);
	wr(CDC_L2_REL_CTRL, rd(CDC_L2_REL_CTRL) | CDC_LN_REL_CTRL_SH_VBLANK);

	/* The reload bit clears once applied: one refresh (25.0 ms) at most. */
	int waited = 0;

	while ((rd(CDC_L2_REL_CTRL) & CDC_LN_REL_CTRL_SH_VBLANK) && waited < 100) {
		k_msleep(1);
		waited++;
	}
	tr_hud_l2_regs[0] = rd(CDC_L2_CTRL);
	tr_hud_l2_regs[1] = rd(CDC_L2_WIN_HPOS);
	tr_hud_l2_regs[2] = rd(CDC_L2_WIN_VPOS);
	tr_hud_l2_regs[3] = rd(CDC_L2_CFB_ADDR);
	tr_hud_l2_regs[4] = rd(CDC_L2_REL_CTRL);
	printk("hud     : layer 2 %ux%u ARGB4444 @0x%08x win h 0x%08x v 0x%08x rel 0x%x (%d ms)\n",
	       (unsigned)win_w,
	       (unsigned)win_h,
	       (unsigned)tr_hud_l2_regs[3],
	       (unsigned)tr_hud_l2_regs[1],
	       (unsigned)tr_hud_l2_regs[2],
	       (unsigned)tr_hud_l2_regs[4],
	       waited);
	if ((tr_hud_l2_regs[4] & CDC_LN_REL_CTRL_SH_VBLANK) ||
	    !(tr_hud_l2_regs[0] & CDC_LN_CTRL_LAYER_EN) || tr_hud_l2_regs[3] != TR_HUD_FB) {
		/* Not provably live: frames stay unflagged, the A32 draws its
		 * sprite HUD. Switch the layer off in case the reload lands late. */
		wr(CDC_L2_CTRL, 0u);
		wr(CDC_L2_REL_CTRL, rd(CDC_L2_REL_CTRL) | CDC_LN_REL_CTRL_SH_VBLANK);
		printk("hud     : layer 2 did not come up -- A32 keeps the HUD\n");
		return false;
	}
	g_up = true;
	return true;
}

bool tr_hud_l2_up(void)
{
	return g_up;
}

/* fix round 7: tr_hp_dbg_read_stable()'s callback shape -- see tr_hp_dbg.h. */
static uint64_t hud_l2_read_busy_cyc(void *ctx)
{
	return ((volatile const hp_dbg_t *)ctx)->busy_cyc;
}

static uint64_t hud_l2_read_total_cyc(void *ctx)
{
	return ((volatile const hp_dbg_t *)ctx)->total_cyc;
}

static void perf(void)
{
	k_thread_runtime_stats_t st;
	tr_perf_raw_t            raw;
	tr_perf_mem_t            mem;

	k_thread_runtime_stats_all_get(&st);
	raw.now_us      = k_cyc_to_us_floor64(k_cycle_get_64());
	raw.flips       = tr_flip_count;
	raw.a32_ticks0  = tr_a32_busy_ticks[0];
	raw.a32_ticks1  = tr_a32_busy_ticks[1];
	raw.he_busy_cyc = st.total_cycles;     /* non-idle */
	raw.he_all_cyc  = st.execution_cycles; /* non-idle + idle */
	/* The HP's own status word (P10's sound ring, SRAM0: powered from reset). */
	raw.hp_magic = ((volatile tr_aring_t *)TR_ARING_ADDR)->magic;
	raw.hp_state = ((volatile tr_aring_t *)TR_ARING_ADDR)->hp_state;
	raw.rail5v_mw =
	    tr_rail5v_avg_mw; /* platform/rail5v_power.c, polled off this frame's hot path */
	/* fix round 5: hp_vision's own beacon (src/ipc/tr_hp_dbg.h), SRAM0, no
	 * cache maintenance needed (this build runs CONFIG_DCACHE=n, same as
	 * every other fixed-address cross-core read in this file). Zeroed/
	 * garbage on a boot with no hp_vision resident (the sound firmware, or
	 * nothing) -- the magic check in hud.c's tr_perf_sample() is what makes
	 * that safe to read unconditionally here. */
	{
		volatile const hp_dbg_t *hpd = (volatile const hp_dbg_t *)TR_MEM_HP_DBG;

		raw.hp_dbg_magic = hpd->magic;
		/* fix round 7: tr_hp_dbg.h's own comment -- busy_cyc/total_cyc have
		 * no seqlock, so a plain volatile read can tear. Double-read-until-
		 * stable needs no writer-side change (both fields are monotonic). */
		raw.hp_busy_cyc  = tr_hp_dbg_read_stable(hud_l2_read_busy_cyc, (void *)hpd);
		raw.hp_total_cyc = tr_hp_dbg_read_stable(hud_l2_read_total_cyc, (void *)hpd);
	}
	/* SRAM: the allocation map (hud.c) with the renderer's real end, read
	 * from its bench block -- only once tr_a32_boot() found SRAM1 powered
	 * (an unpowered SRAM1 bus-faults); otherwise its 768 KiB image + .bss budget. */
	mem.sram_used =
	    tr_mem_sram_used(tr_a32_link_ok() ? *(volatile uint32_t *)TR_RENDER_IMG_END_ADDR : 0u);
	mem.sram_total = TR_MEM_SRAM_TOTAL;
	mem.itcm       = (uint32_t)(uintptr_t)_flash_used;
	mem.dtcm       = (uint32_t)(uintptr_t)_image_ram_size;
	/* IMG: this HE image + the renderer payload it LAUNCHes (release builds
	 * know its length; the stub, TF-A and the HP stub are not counted). In
	 * the dev flow the HE is RAM-run, so this is image size, not MRAM use. */
#if TR_M55_AUTOLAUNCH
	mem.img = mem.itcm + tr_a32_autolaunch_id[1];
#else
	mem.img = mem.itcm;
#endif
	tr_perf_sample(&g_perf, &raw, &mem, &g_view);
}

void tr_hud_l2_present(const tr_score_t    *s,
                       uint8_t              banner,
                       bool                 attract,
                       uint8_t              invite,
                       const tr_zone_t     *z,
                       uint8_t              character,
                       const tr_hiscore_t  *hs,
                       const tr_initials_t *ini)
{
	if (!g_up) {
		return;
	}
	uint32_t t0 = k_cycle_get_32();

	tr_hud_view_set(&g_view, s, banner, attract, invite);
	tr_hud_view_zone(&g_view, z->zone, z->seq);
	tr_hud_view_booth(&g_view, hs, ini);
	g_view.character = character;
	perf();

	uint32_t px = tr_hud_update(&g_hud, g_fb, &g_view, NULL);
	uint32_t dt = k_cycle_get_32() - t0;

	tr_hud_cyc_last = dt;
	tr_hud_cyc_max  = dt > tr_hud_cyc_max ? dt : tr_hud_cyc_max;
	tr_hud_cyc_total += dt;
	tr_hud_px_total += px;
	tr_hud_updates++;
	tr_hud_repaints += px != 0u;

	const struct cdc200_data *cd = DEVICE_DT_GET(DT_NODELABEL(cdc200))->data;

	tr_cdc_fifo_underruns = cd->fifo_underrun_count;
	tr_cdc_bus_errs       = cd->bus_err_count;
}
