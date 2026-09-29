#include <string.h>

#include <alp/display.h>
#include <alp/peripheral.h>
#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

/*
 * SDK GAP -- the one place this project reaches past <alp/display.h>.
 *
 * Zephyr's display API has no page-flip concept and neither does
 * <alp/display.h>: alp_display_blit() resolves to display_write(), which the
 * CDC200 driver services by memcpy-ing into the framebuffer it is CURRENTLY
 * SCANNING OUT.  With one buffer, tr_render_frame()'s erase pass is therefore
 * visible on the glass: for the width of that pass every moved sprite is gone,
 * and any scanout crossing it shows a frame with holes in it.  That is the
 * flicker the maintainer saw on E1M-AEN803 2026W36-0009.
 *
 * The fork's CDC200 driver already has the missing piece as a device-specific
 * extension (cdc200_swap_fb / cdc200_get_framebuffer / restore_fb), so this
 * file uses it directly rather than poking 0x49031134 itself -- see
 * tr_display_flip() for the reload semantics that makes that the right call.
 * The header is private to the driver directory, so CMakeLists.txt puts that
 * directory on this app's include path.  probe/fillrate/src/main.c set the
 * precedent of calling a Zephyr display function the alp-sdk surface does not
 * cover; this is the same gap, one step deeper.  CLOSE IT with an alp-sdk
 * change -- a flip/page-swap op on <alp/display.h> -- and this include, the
 * CMake line and the register-level knowledge below all go away.
 */
#include <display_cdc200.h>

#include "display.h"

/*
 * The ONLY file in this project that talks to the display API.  Everything
 * else works in game coordinates, so swapping panels or SDK versions touches
 * exactly one file.
 */
static alp_display_t     *g_disp;
static alp_display_caps_t g_caps;

/* The CDC200 behind alp-display0 -- the flip target; see the SDK-gap note. */
static const struct device *g_cdc;

/*
 * The two framebuffers.  [0] is the CDC200 node's own memory-region (the
 * shield's lcd_fb, 2 MiB at the top of SRAM0); [1] is its twin in SRAM1.
 *
 * SRAM1 and not SRAM0: the shield's lcd_fb (2 MiB at 0x02200000) plus the
 * sram0 partition (2 MiB at 0x02000000) already account for the whole 4 MiB
 * of SRAM0, so a second frame does not fit there.  SRAM1 is a separate 4 MiB
 * bank, wholly unused by this project (nothing is linked into it; the video
 * buffer pool is pinned to SRAM0 by prj.conf).  Used BY ADDRESS, exactly as
 * the driver uses lcd_fb and as probe/fillrate/src/main.c uses SRAM1, so it
 * costs the image nothing -- it is not linked and does not appear in the
 * RAM-run .bin.
 */
#define TR_SRAM1_NODE DT_NODELABEL(sram1)

/*
 * Double buffering is OFF by default until SRAM1 is proven powered.
 *
 * "Wholly unused" above is true of the linker map and false of the silicon.
 * On E1M-AEN803 2026W36-0009, measured cold with no image loaded, SRAM1 does
 * not answer at all: 0x023FFFF0 (the top of SRAM0) reads back, 0x02400000
 * returns "Failed to read memory" to the DEBUGGER as well as bus-faulting the
 * core -- so it is unpowered, not merely unmapped by the MPU. With this on,
 * the image bus-faults inside memcpy during display bring-up before a single
 * frame runs.
 *
 * SRAM1 is a power-gated memory block the Secure Enclave controls
 * (POWER_MEM_SRAM_1_ENABLE via SERVICES_power_memory_req(), or MB_SRAM1 in
 * the run profile's memory_blocks). Powering it is not yet implemented or
 * proven, and the run-profile route carries a known hazard -- see
 * mipi_display_e8.c's "LATENT HAZARD" note: the display glue powers the
 * D-PHY by direct register write at PRE_KERNEL_1 priority 0, before the SE
 * service is even up, so a later whole-profile set_run_cfg can power the
 * D-PHY down under a live panel. Set this to 1 only once SRAM1 is powered by
 * a memory-only request that leaves PHY and IP clock gating alone.
 *
 * With it off, the flip below is a no-op and drawing goes straight into the
 * live buffer: correct, playable, and it flickers -- the known-working state.
 */
/*
 * Where the back buffer goes follows whether a camera is built in.
 *
 * Display-only build (no CONFIG_VIDEO): the 2 MiB SRAM0 partition below
 * lcd_fb is empty, so the back buffer sits at its base -- 1,843,200 B from
 * 0x02000000 ends at 0x021C2000, clear of lcd_fb at 0x02200000. Both
 * framebuffers are then in powered SRAM0 and double buffering is safe, so it
 * defaults ON. This is Alif's own validated design for this silicon: the
 * DFP's lvgl_port_disp.c double-buffers with two full-screen frames and
 * requests only `MRAM_MASK | SRAM0_MASK` -- it never powers SRAM1.
 *
 * Camera build (CONFIG_VIDEO): the 512 KiB video buffer pool takes the base
 * of that partition, and two frames plus the pool overshoot SRAM0 by exactly
 * 16 KiB (4,194,304 - 2 x 1,843,200 - 524,288). The back buffer would then
 * need SRAM1, which is unpowered, so it defaults OFF -- see above.
 */
#if IS_ENABLED(CONFIG_VIDEO)
#define TR_BACK_NODE             TR_SRAM1_NODE
#define TR_DOUBLE_BUFFER_DEFAULT 0
#else
#define TR_BACK_NODE             DT_NODELABEL(sram0)
#define TR_DOUBLE_BUFFER_DEFAULT 1
#endif

#ifndef TR_DOUBLE_BUFFER
#define TR_DOUBLE_BUFFER TR_DOUBLE_BUFFER_DEFAULT
#endif

#define TR_FB_BYTES \
	((size_t)DT_PROP(DT_NODELABEL(cdc200), width) * DT_PROP(DT_NODELABEL(cdc200), height) * 2u)
BUILD_ASSERT(TR_FB_BYTES <= DT_REG_SIZE(TR_BACK_NODE),
             "the back buffer must fit its bank -- there is no external memory on this board");

static uint8_t *g_fb[2];
static size_t   g_fb_size;

/*
 * Which of the two this project DRAWS into.  Starts at 1 (the SRAM1 twin) so
 * that the first thing drawn after startup -- tr_render_init()'s full-panel
 * background paint -- lands in the buffer that is NOT yet on screen, and the
 * first flip therefore shows a fully painted frame.  Starting at 0 would paint
 * the live buffer (no better than single-buffering) and then flip to a SRAM1
 * twin that nothing had initialised: one frame of uninitialised SRAM on the
 * glass.  The flip's copy-back (below) initialises the OTHER buffer from the
 * same paint, so both are valid from the first flip on.
 */
static unsigned g_back = 1u;

/* Cleared for good if a queued flip never lands -- see flip_gave_up(). */
static bool g_flip_ok;

/* One-shot latch: a per-frame print would bury the 8,191-byte RAM console. */
static bool g_blit_err_printed;

/*
 * WHY A DIRTY-RECT LIST, and not just two buffers.
 *
 * render.c draws INCREMENTALLY: it erases only what moved and redraws only
 * the sprites, trusting that the buffer already holds the rest of the frame
 * (background, lane markers, everything that did not move).  Plain
 * alternation breaks that trust -- the buffer drawn into is two frames old,
 * so the erase rectangles (recorded from frame N-1) no longer match the
 * sprites actually present (from frame N-2), and every sprite leaves a
 * ghost.  So after each flip the rectangles this frame touched are copied
 * from the new front to the new back, which makes the two buffers identical
 * again and keeps render.c's invariant true.  That copy is the same number
 * of bytes the frame's own blits were -- NOT a 1,843,200 B full-frame copy.
 *
 * Sized for the worst tick: tr_render_init()'s full-panel repaint (8 x 14 =
 * 112 chunks + 2 lane markers) plus tr_render_frame() (1 + 16 erases, 1 + 16
 * draws, 3 HUD digits) plus tr_render_banner() (5 plate + 1 trace + 5 text)
 * = 162.  Overflow is not corruption, just the slow path: the whole frame is
 * copied instead.
 */
#define TR_DIRTY_MAX 192

struct tr_rect {
	uint16_t x, y, w, h;
};

static struct tr_rect g_dirty[TR_DIRTY_MAX];
static unsigned       g_dirty_n;
static bool           g_dirty_all;

/*
 * How long a queued flip is given to land before this file gives up on
 * double buffering entirely.  The panel refreshes at 40.0 Hz (25.0 ms), and
 * the swap is applied by the driver's scanline ISR, so one refresh period is
 * the expected wait and anything past four means the ISR is not running.
 */
#define TR_FLIP_TIMEOUT_MS 100

/*
 * Returns 0 on success, -1 for the ~1-in-8-to-10 INTERMITTENT panel init
 * defect (alp_display_open() itself returned NULL -- worth a caller
 * retrying, e.g. main.c), or -2 for a genuine, DETERMINISTIC configuration
 * failure (caps unavailable, or a pixel format this project cannot draw
 * to) that retrying cannot fix.
 *
 * The distinction matters to a caller that retries: blindly retrying a -2
 * used to leak a handle from the SDK's fixed display-handle pool on every
 * attempt (the two -2 paths got a handle from alp_display_open() before
 * failing later, and never closed it) and, once the pool was exhausted,
 * misreport the resulting NOMEM as the intermittent panel defect instead
 * of the real cause (whole-branch fix round B, B1). Both -2 paths now
 * close the handle they got before returning.
 *
 * The -1 path deliberately does NOT print the "RESULT FAIL:" token (also
 * used by camera.c, and grepped for as the bench-log terminal-failure
 * marker): from here, "this one attempt failed" is not yet "this boot
 * failed" -- a retrying caller decides when to say that, so it is the one
 * that prints RESULT FAIL: if it gives up. The two -2 paths DO print it
 * immediately: they are not retried, so this IS the final word on them.
 *
 * Failing to set the second buffer up is NOT one of those failures: it costs
 * the flicker fix, not the game, so it degrades to single-buffered drawing
 * (the behaviour before this file grew a back buffer) and says so.
 */
int tr_display_open(void)
{
	alp_display_config_t  cfg = ALP_DISPLAY_CONFIG_DEFAULT(0);
	struct cdc200_fb_desc fb;

	g_disp = alp_display_open(&cfg);
	if (g_disp == NULL) {
		/* The panel's init fails on roughly 1 cold boot in 8-10 and Zephyr
		 * has no re-init path -- say so, but leave RESULT FAIL: to a
		 * caller that has actually given up retrying (see this function's
		 * comment). */
		printk("display : open failed -- known intermittent panel init defect (retryable)\n");
		return -1;
	}
	if (alp_display_get_caps(g_disp, &g_caps) != ALP_OK) {
		printk("RESULT FAIL: display caps unavailable\n");
		alp_display_close(g_disp);
		g_disp = NULL;
		return -2;
	}
	if (g_caps.format != ALP_PIXFMT_RGB565) {
		/* Every draw path below assumes 2 bytes per pixel. */
		printk("RESULT FAIL: expected ALP_PIXFMT_RGB565 (%d), panel reports %d\n",
		       (int)ALP_PIXFMT_RGB565,
		       (int)g_caps.format);
		alp_display_close(g_disp);
		g_disp = NULL;
		return -2;
	}

	/*
	 * The front buffer is taken from the driver rather than from the DT
	 * address, so its SIZE comes from the same place cdc200_swap_fb()
	 * validates against -- it rejects a descriptor whose fb_size does not
	 * match the layer's, and a silently rejected swap is a frozen screen.
	 */
	g_cdc = DEVICE_DT_GET(DT_NODELABEL(cdc200));
	cdc200_get_framebuffer(g_cdc, CDC_LAYER_1, &fb);
	g_fb[0]   = fb.fb_addr;
	g_fb[1]   = (uint8_t *)DT_REG_ADDR(TR_BACK_NODE);
	g_fb_size = fb.fb_size;

	g_flip_ok = TR_DOUBLE_BUFFER && device_is_ready(g_cdc) && (fb.fb_addr != NULL) &&
	            (fb.fb_size == TR_FB_BYTES);
#if !TR_DOUBLE_BUFFER
	/* See TR_DOUBLE_BUFFER's comment: with a camera built in, the back buffer
	 * would have to live in SRAM1, which is power-gated and unpowered at boot
	 * on this board. Never dereference g_fb[1] as SRAM1 -- even a read
	 * bus-faults. */
	printk(
	    "display : double buffering OFF (TR_DOUBLE_BUFFER=0) -- camera build, SRAM1 not powered\n");
#endif
	if (!g_flip_ok) {
		/* Not RESULT FAIL: -- the game still runs, it just flickers. */
		printk("display : no back buffer (cdc ready=%d fb=%p size=%u want=%u) -- "
		       "single-buffered, expect flicker\n",
		       (int)device_is_ready(g_cdc),
		       (void *)fb.fb_addr,
		       (unsigned)fb.fb_size,
		       (unsigned)TR_FB_BYTES);
		g_fb[1] = g_fb[0];
		g_back  = 0u; /* draw straight into the live buffer, as before */
	}

	printk("display : %ux%u RGB565 front=%p back=%p\n",
	       g_caps.width,
	       g_caps.height,
	       (void *)g_fb[g_back ^ 1u],
	       (void *)g_fb[g_back]);
	return 0;
}

uint16_t tr_display_width(void)
{
	return g_caps.width;
}

uint16_t tr_display_height(void)
{
	return g_caps.height;
}

/*
 * Copy a rectangle between the two framebuffers, `from` -> `to`, and clean it
 * out of the D-cache.  CONFIG_DCACHE=y on this target and the CDC200 fetches
 * through its own AXI master, so a written line still sitting in the M55's
 * cache is a line the scanout does not see -- the driver's own write path
 * flushes for exactly this reason.  Flushed per row, not over the whole
 * bounding span: a 96 x 96 sprite is 96 x 192 B of real pixels but spans
 * 96 x 1,440 B of framebuffer, and cleaning the span would do 7x the work.
 */
static void
copy_rect(uint8_t *to, const uint8_t *from, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	size_t         stride = (size_t)g_caps.width * 2u;
	size_t         row_b  = (size_t)w * 2u;
	size_t         off    = (size_t)y * stride + (size_t)x * 2u;
	uint8_t       *dst    = to + off;
	const uint8_t *src    = from + off;

	for (uint16_t row = 0; row < h; row++) {
		memcpy(dst, src, row_b);
		sys_cache_data_flush_range(dst, row_b);
		dst += stride;
		src += stride;
	}
}

int tr_display_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void *px)
{
	const uint8_t *src    = px;
	size_t         stride = (size_t)g_caps.width * 2u;
	size_t         row_b  = (size_t)w * 2u;
	uint8_t       *dst;

	/*
	 * render.c's paint() is the bounds authority for the game, but this is
	 * the last gate before a raw pointer write: the SDK's own blit used to
	 * do this check, and going around it (see the SDK-gap note) means doing
	 * it here or not at all.
	 */
	if ((uint32_t)x + w > g_caps.width || (uint32_t)y + h > g_caps.height) {
		if (!g_blit_err_printed) {
			g_blit_err_printed = true;
			printk("display: first blit rejected -- out of bounds at (%u,%u,%ux%u)\n", x, y, w, h);
		}
		return -1;
	}

	dst = g_fb[g_back] + (size_t)y * stride + (size_t)x * 2u;
	for (uint16_t row = 0; row < h; row++) {
		memcpy(dst, src, row_b);
		sys_cache_data_flush_range(dst, row_b);
		dst += stride;
		src += row_b;
	}

	if (g_dirty_n == ARRAY_SIZE(g_dirty)) {
		g_dirty_all = true; /* slow path: the whole frame gets copied back */
	} else {
		g_dirty[g_dirty_n++] = (struct tr_rect){ x, y, w, h };
	}
	return 0;
}

/*
 * A queued swap never reached the register: the driver's scanline ISR is the
 * only thing that applies one, so it is not running.  Carrying on alternating
 * would leave every second frame pointed at a buffer the CDC never reads --
 * far worse than the flicker this file exists to remove -- so stop flipping,
 * hand the queued swap back (restore_fb() re-points the driver's pending
 * framebuffer at its DT default), and draw straight into the live buffer from
 * here.  That is exactly the pre-double-buffer behaviour: the game keeps
 * running and flickers.
 */
static void flip_gave_up(void)
{
	restore_fb(g_cdc);
	g_flip_ok = false;
	g_back    = 0u; /* lcd_fb: what the register still holds, the ISR never having moved it */
	printk("display: framebuffer swap never landed in %d ms -- CDC200 scanline IRQ silent; "
	       "single-buffered from here (flicker returns, game keeps running)\n",
	       TR_FLIP_TIMEOUT_MS);
}

/*
 * THE FLIP, and why it is a driver call and not a write to 0x49031134.
 *
 * CDC_L1_CFB_ADDR (0x49031134) is a SHADOWED register on this TES CDC-2.1: a
 * write to it changes nothing until a shadow reload is triggered through
 * CDC_SRCTRL (0x49031024) -- bit 0 (CDC_SRCTRL_IMR) reloads immediately,
 * bit 1 (CDC_SRCTRL_VBR) at the next vertical blanking.  Poking the address
 * register alone would do nothing at all; poking it and then forcing the
 * immediate reload from here, at an arbitrary point in the frame, would swap
 * the scanout source mid-scanout and trade the flicker for a tear.
 *
 * The driver already does it correctly and this call rides on that:
 * cdc200_swap_fb() only records the request (data->next_fb).  The driver arms
 * its line IRQ at line vsync_len + vbp + active_height = 2 + 14 + 1280 = 1296
 * of a 1,312-line frame -- i.e. the instant active video ends and vertical
 * blanking begins -- and cdc200_isr() writes CDC_L1_CFB_ADDR and triggers the
 * immediate reload from THERE.  So the reload is immediate but it happens
 * inside the ~610 us blanking window (32 blanked lines x 19.05 us at the
 * 40 MHz pixel clock), which is a tear-free swap.
 *
 * The wait below is therefore also the vsync: cdc200_isr() publishes the
 * applied address through data->curr_fb, which cdc200_get_framebuffer() reads
 * back, so "the swap has landed" and "we are in the blanking interval that
 * applied it" are the same instant.  It has to be waited for, not assumed:
 * the buffer this frees is the one drawn into next, and drawing into it one
 * refresh early is the very artefact being fixed.
 */
/* Bench-readable over the AHB-AP (non-static so `nm` names them): completed
 * swaps, and ticks that had nothing to show.  flips/s is the frame rate. */
volatile uint32_t tr_flip_count;
volatile uint32_t tr_flip_idle_count;
/* Cycles spent waiting for the swap to land, and copying the new back buffer
 * level with the screen afterwards. */
volatile uint64_t tr_cyc_flip_wait;
volatile uint64_t tr_cyc_copyback;

void tr_display_flip(void)
{
	struct cdc200_fb_desc fb     = { .fb_addr = g_fb[g_back], .fb_size = g_fb_size };
	unsigned              waited = 0;

	if (!g_flip_ok || (g_dirty_n == 0u && !g_dirty_all)) {
		/* Nothing drawn since the last flip: the two buffers already
		 * agree (the copy-back below guarantees it), so there is nothing
		 * to show and no reason to spend a blanking interval. */
		g_dirty_n   = 0u;
		g_dirty_all = false;
		tr_flip_idle_count++;
		return;
	}

	uint64_t t0 = k_cycle_get_64();

	cdc200_swap_fb(g_cdc, CDC_LAYER_1, &fb);

	for (;;) {
		struct cdc200_fb_desc live;

		cdc200_get_framebuffer(g_cdc, CDC_LAYER_1, &live);
		if (live.fb_addr == g_fb[g_back]) {
			break;
		}
		if (waited++ >= TR_FLIP_TIMEOUT_MS) {
			flip_gave_up();
			g_dirty_n   = 0u;
			g_dirty_all = false;
			return;
		}
		k_msleep(1);
	}

	g_back ^= 1u;
	tr_flip_count++;

	uint64_t t1 = k_cycle_get_64();

	tr_cyc_flip_wait += t1 - t0;

	/* Bring the new back buffer level with what is now on screen -- see the
	 * dirty-rect note above for why render.c cannot work without this. */
	if (g_dirty_all) {
		copy_rect(g_fb[g_back], g_fb[g_back ^ 1u], 0, 0, g_caps.width, g_caps.height);
	} else {
		for (unsigned i = 0; i < g_dirty_n; i++) {
			copy_rect(g_fb[g_back],
			          g_fb[g_back ^ 1u],
			          g_dirty[i].x,
			          g_dirty[i].y,
			          g_dirty[i].w,
			          g_dirty[i].h);
		}
	}
	tr_cyc_copyback += k_cycle_get_64() - t1;
	g_dirty_n   = 0u;
	g_dirty_all = false;
}

int tr_display_clear(void)
{
	return (alp_display_clear(g_disp) == ALP_OK) ? 0 : -1;
}
