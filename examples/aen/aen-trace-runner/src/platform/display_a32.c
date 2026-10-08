/*
 * src/platform/display_a32.c -- the display side of TR_RENDER=A32.
 *
 * Replaces display.c in the A32 build (CMakeLists.txt picks one). The M55
 * writes NO pixels here: the A32 renderer draws every frame in full into the
 * buffer the M55 names as free, and this file only brings the panel up and
 * swaps the CDC200 to the finished buffer. No blit, no copy-back, no cache
 * maintenance -- CONFIG_DCACHE=n and the A32 maps the framebuffers
 * non-cacheable (plan section 3), so there is nothing to clean.
 *
 * The flip rides the same CDC200 driver extension as display.c (see its
 * SDK-GAP note and tr_display_flip() for the shadow-register / line-1296
 * ISR reasoning): cdc200_swap_fb() queues, the ISR applies it in vertical
 * blanking, curr_fb read back through cdc200_get_framebuffer() says it landed.
 */
#include <alp/display.h>
#include <alp/peripheral.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/toolchain.h>

#include <display_cdc200.h>

#include "../ipc/tr_flip.h"
#include "../render/panel_rot.h"
#include "display.h"

/* The scan-out layer-1 window, whatever the panel: the full panel on RK055, a
 * 1280 x 720 window of the RVT121's 1280 x 800 (the shield-fit overlay,
 * shield-fit/e1m_evk_rvt121hvdfwca0.overlay). It is always the portrait
 * frame's byte count: a panel mounted turned scans the same bytes the A32
 * writes rotated (render/panel_rot.h). */
#define TR_L1_NODE DT_NODELABEL(cdc200)
#define TR_L1_W \
	(DT_PROP_OR(TR_L1_NODE, win_x1_l1, DT_PROP(TR_L1_NODE, width)) - \
	 DT_PROP_OR(TR_L1_NODE, win_x0_l1, 0))
#define TR_L1_H \
	(DT_PROP_OR(TR_L1_NODE, win_y1_l1, DT_PROP(TR_L1_NODE, height)) - \
	 DT_PROP_OR(TR_L1_NODE, win_y0_l1, 0))
#define TR_FB_BYTES ((size_t)TR_L1_W * TR_L1_H * 2u)

/* The window is the 720 x 1280 portrait content as scanned (rotation 0) or turned
 * (90 / 270: 1280 x 720): the right shape for the mount-rotation, not just the
 * right byte count. */
#define TR_MOUNT_ROT DT_PROP_OR(TR_L1_NODE, mount_rotation, 0)
BUILD_ASSERT(TR_MOUNT_ROT == 0 || TR_MOUNT_ROT == 90 || TR_MOUNT_ROT == 270,
             "mount-rotation must be 0, 90 or 270 (the renderer cannot produce 180)");
BUILD_ASSERT(TR_MOUNT_ROT == 0 ? (TR_L1_W == TR_ROT_PORTRAIT_W && TR_L1_H == TR_ROT_PORTRAIT_H)
                               : (TR_L1_W == TR_ROT_PORTRAIT_H && TR_L1_H == TR_ROT_PORTRAIT_W),
             "layer-1 window is not 720x1280 (mount-rotation 0) / 1280x720 (90, 270): add or fix "
             "the shield's shield-fit overlay");

/* FB A is the SRAM0 partition base (plan section 4); FB B is SRAM1
 * 0x025EA000 (tr_mbox.h), outside every DT partition -- cdc200_swap_fb()
 * checks only the size. */
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(sram0)) == TR_FB_A,
             "TR_FB_A must be the sram0 partition base");
BUILD_ASSERT(TR_FB_BYTES <= DT_REG_SIZE(DT_NODELABEL(sram0)), "FB A must fit the sram0 partition");
BUILD_ASSERT(TR_FB_A + TR_FB_SLOT_SIZE <= TR_FB_B, "FB A's slot must end before FB B");
BUILD_ASSERT(TR_FB_BYTES == TR_FB_SIZE, "panel size != tr_mbox.h TR_FB_SIZE");

/* Same bound as display.c: one refresh is 25.0 ms (33.3 at TR_PANEL_HZ 30), three or four means the ISR is silent. */
#define TR_FLIP_TIMEOUT_MS 100

static alp_display_t       *g_disp;
static alp_display_caps_t   g_caps;
static const struct device *g_cdc;
static size_t               g_fb_size;
static tr_flip_pace_t       g_pace;
static bool                 g_land_err_printed;

/* Bench-readable over the AHB-AP (non-static so `nm` names them). flips/s is
 * the frame rate; overruns are landed flips more than 1.5 refreshes apart. */
volatile uint32_t tr_flip_count;
volatile uint32_t tr_frame_overrun_count;
/* Landed-flip intervals by refreshes taken (tr_flip.h TR_FLIP_HIST_N buckets:
 * 40 Hz <=27.5, <=52.5, <=77.5, ... ms, [7] > 177.5 ms; 30 Hz <=35.8, <=69.2,
 * ... ms, [7] > 235.8 ms), not counting intervals that span a ui_hold / pace
 * reset; tr_flip_intervals_total is their sum. */
volatile uint32_t tr_flip_hist[TR_FLIP_HIST_N];
volatile uint32_t tr_flip_intervals_total;

/* Same -1 (retry-worthy) / -2 (deterministic) contract as display.c's. */
int tr_display_open(void)
{
	alp_display_config_t  cfg = ALP_DISPLAY_CONFIG_DEFAULT(0);
	struct cdc200_fb_desc fb;

	g_disp = alp_display_open(&cfg);
	if (g_disp == NULL) {
		printk("display : open failed -- known intermittent panel init defect (retryable)\n");
		return -1;
	}
	if (alp_display_get_caps(g_disp, &g_caps) != ALP_OK || g_caps.format != ALP_PIXFMT_RGB565) {
		printk("RESULT FAIL: display caps unavailable or not RGB565\n");
		alp_display_close(g_disp);
		g_disp = NULL;
		return -2;
	}
	if (!tr_rot_valid(g_caps.rotation)) {
		printk("RESULT FAIL: display mount-rotation %u is not 0, 90 or 270\n",
		       (unsigned)g_caps.rotation);
		alp_display_close(g_disp);
		g_disp = NULL;
		return -2;
	}

	/* fb_size from the driver: cdc200_swap_fb() silently rejects any other. */
	g_cdc = DEVICE_DT_GET(DT_NODELABEL(cdc200));
	if (!device_is_ready(g_cdc)) {
		printk("RESULT FAIL: CDC200 not ready\n");
		alp_display_close(g_disp);
		g_disp = NULL;
		return -2;
	}
	cdc200_get_framebuffer(g_cdc, CDC_LAYER_1, &fb);
	g_fb_size = fb.fb_size;
	/* The CDC200 boots scanning the shield's lcd_fb (0x02200000), which is
	 * not FB B any more (tr_mbox.h: SRAM1 0x025EA000) and which the A32
	 * renderer uses as cached scratch: park on FB A so the live/free pair
	 * is valid from here on. FB A shows whatever it holds until the A32's
	 * first frame. */
	if (!tr_fb_valid((uint32_t)(uintptr_t)fb.fb_addr) && fb.fb_size == TR_FB_BYTES &&
	    tr_display_flip_to(TR_FB_A) == 0) {
		cdc200_get_framebuffer(g_cdc, CDC_LAYER_1, &fb);
	}
	if (!tr_fb_valid((uint32_t)(uintptr_t)fb.fb_addr) || fb.fb_size != TR_FB_BYTES) {
		printk("RESULT FAIL: CDC200 not flippable (fb=%p size=%u want=%u)\n",
		       (void *)fb.fb_addr,
		       (unsigned)fb.fb_size,
		       (unsigned)TR_FB_BYTES);
		/* Which half is silent: the controller (CDC_EN in GLB_CTRL bit 0,
		 * the line IRQ unmasked / latched, the scan position moving) or the
		 * NVIC (the line IRQ enabled / pending on this core). */
		uintptr_t r    = DT_REG_ADDR(DT_NODELABEL(cdc200));
		uint32_t  pos0 = sys_read32(r + CDC_POS_STAT);

		k_busy_wait(1000);
		printk("display : CDC200 GLB_CTRL 0x%08x IRQ_MASK0 0x%08x IRQ_STATUS0 0x%08x POS 0x%08x -> "
		       "0x%08x "
		       "irq %d en %d pend %d\n",
		       sys_read32(r + CDC_GLB_CTRL),
		       sys_read32(r + CDC_IRQ_MASK0),
		       sys_read32(r + CDC_IRQ_STATUS0),
		       pos0,
		       sys_read32(r + CDC_POS_STAT),
		       DT_IRQN(DT_NODELABEL(cdc200)),
		       irq_is_enabled(DT_IRQN(DT_NODELABEL(cdc200))),
		       NVIC_GetPendingIRQ(DT_IRQN(DT_NODELABEL(cdc200))));
		alp_display_close(g_disp);
		g_disp = NULL;
		return -2;
	}

	printk("display : %ux%u RGB565, A32 renders, live=%p\n",
	       g_caps.width,
	       g_caps.height,
	       (void *)fb.fb_addr);
	return 0;
}

/* The game's own geometry: always the portrait content (the panel's own
 * geometry, and any mount-rotation, is the producer's to turn: panel_rot.h). */
uint16_t tr_display_width(void)
{
	return TR_ROT_PORTRAIT_W;
}

uint16_t tr_display_height(void)
{
	return TR_ROT_PORTRAIT_H;
}

uint16_t tr_display_rotation(void)
{
	return g_caps.rotation;
}

static uint32_t live_fb(void)
{
	struct cdc200_fb_desc live;

	cdc200_get_framebuffer(g_cdc, CDC_LAYER_1, &live);
	return (uint32_t)(uintptr_t)live.fb_addr;
}

uint32_t tr_display_free_fb(void)
{
	return tr_fb_free(live_fb());
}

int tr_display_flip_to(uint32_t fb_addr)
{
	uint32_t live = live_fb();

	if (!tr_fb_valid(fb_addr) || fb_addr == live) {
		return -1; /* never swap to a stray address, or to what is already on screen */
	}

	struct cdc200_fb_desc fb = { .fb_addr = (uint8_t *)(uintptr_t)fb_addr, .fb_size = g_fb_size };

	cdc200_swap_fb(g_cdc, CDC_LAYER_1, &fb);
	/* 100 us poll (one tick at 10 kHz): the A32 gets the next buffer this
	 * much sooner after the vblank than with a 1 ms poll. */
	for (int64_t deadline = k_uptime_get() + TR_FLIP_TIMEOUT_MS; live_fb() != fb_addr;) {
		if (k_uptime_get() >= deadline) {
			/* ISR silent: withdraw the request so a late ISR cannot swap
			 * under a buffer the A32 is about to be handed. */
			fb.fb_addr = (uint8_t *)(uintptr_t)live;
			cdc200_swap_fb(g_cdc, CDC_LAYER_1, &fb);
			/* The swap may have landed between the check above and the
			 * withdrawal; then the withdrawal is itself a pending swap
			 * back to `live`, and the free buffer read now would be the
			 * one about to go live. Wait for it to settle (bounded: a
			 * silent ISR leaves `live` in place, which is also safe). */
			for (int settle = 0; live_fb() != live && settle < TR_FLIP_TIMEOUT_MS; settle++) {
				k_msleep(1);
			}
			if (!g_land_err_printed) {
				g_land_err_printed = true;
				printk("display: swap never landed in %d ms -- CDC200 scanline IRQ silent\n",
				       TR_FLIP_TIMEOUT_MS);
			}
			return -1;
		}
		k_usleep(100);
	}

	uint64_t now_us = k_cyc_to_us_floor64(k_cycle_get_64());

	tr_flip_count++;
	tr_flip_hist_note(&g_pace, now_us, tr_flip_hist, &tr_flip_intervals_total);
	if (tr_flip_pace_landed(&g_pace, now_us)) {
		tr_frame_overrun_count++;
	}
	return 0;
}

void tr_display_pace_reset(void)
{
	tr_flip_pace_reset(&g_pace);
}
