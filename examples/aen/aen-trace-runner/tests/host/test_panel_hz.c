/* tests/host/test_panel_hz.c -- the 30 Hz panel option (src/game/panel_hz.h).
 *
 * runner.sh builds this twice: default (TR_PANEL_HZ 40, the host default and
 * the RK055 shield's timing) and -DTR_PANEL_HZ=30 (what the game derives for
 * panel_30hz.overlay and the Riverdi shield). Checks, per build:
 *  1. the panel timing arithmetic -- refresh = pclk / (htotal * vtotal) --
 *     for the shield's timing and for panel_30hz.overlay as committed (the
 *     file is parsed, not copied), inside every limit the link has;
 *  2. every frame-counted constant keeps its real-time length at the build's
 *     refresh: game pace, crash, tilt holds, HUD popup / blink, and the
 *     crash_tick the A32 renderer animates from (it counts 40 Hz frames). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../src/game/attract.h"
#include "../../src/game/panel_hz.h"
#include "../../src/game/state.h"
#include "../../src/game/tilt.h"
#include "../../src/hud/hud.h"
#include "../../src/ipc/tr_flip.h"
#include "../../src/ipc/tr_mbox.h"

/* The shield's timing (zephyr/boards/shields/e1m_evk_rk055hdmipi4ma0/
 * e1m_evk_rk055hdmipi4ma0.overlay, &cdc200). */
#define W         720
#define HSYNC     6
#define HFP       12
#define HBP       24
#define H         1280
#define VSYNC     2
#define VFP       16
#define VBP       14
#define PCLK      40000000.0
#define SYST_ACLK 400000000.0

/* `prop = <N>;` from the overlay, -1 if absent. */
static long overlay_prop(const char *path, const char *prop)
{
	FILE *f = fopen(path, "r");
	char  line[256];
	long  v = -1;

	assert(f);
	while (fgets(line, sizeof(line), f)) {
		char *p = strstr(line, prop);

		if (p && p[strlen(prop)] == ' ' && sscanf(p + strlen(prop), " = <%ld>;", &v) == 1) {
			break;
		}
	}
	fclose(f);
	return v;
}

/* Refresh and the checks every timing must pass. */
static double timing(double pclk, int vfp)
{
	int    htotal = W + HSYNC + HFP + HBP, vtotal = H + VSYNC + vfp + VBP;
	double hz = pclk / ((double)htotal * vtotal);
	/* dsi_dw.c non-burst lane rate: ((pkt*24/8 + 12)/pkt) * pclk * 8/lanes
	 * + (32/lanes) * pclk / hactive, pkt = 720, 2 lanes, RGB888 link. */
	double lane = ((W * 24.0 / 8.0 + 12.0) / W) * pclk * (8.0 / 2.0) + (32.0 / 2.0) * pclk / W;
	double div  = SYST_ACLK / pclk;

	assert(fabs(div - round(div)) < 1e-9 && div >= 2.0 &&
	       div <= 511.0);             /* mipi_display_e8.c BUILD_ASSERTs */
	assert(lane <= 500e6);            /* panel-max-lane-bandwidth: the 2-lane ceiling */
	assert(vfp <= 1023);              /* DSI_VID_VFP_LINES is 10 bits */
	assert(VSYNC + VBP + H < vtotal); /* the CDC200 line IRQ (the flip) lands in blanking */
	printf("  pclk %.0f Hz, %d x %d total, %.4f Hz, line %.1f kHz, lane %.2f Mbps\n",
	       pclk,
	       htotal,
	       vtotal,
	       hz,
	       pclk / htotal / 1e3,
	       lane / 1e6);
	return hz;
}

/* |a - b| <= tol. */
static int near(double a, double b, double tol)
{
	return fabs(a - b) <= tol;
}

int main(void)
{
	const double hz = TR_PANEL_HZ, frame_s = 1.0 / hz;

	printf("TR_PANEL_HZ %d\n", TR_PANEL_HZ);

	/* 1. Timing. The shield: 40.0 Hz. */
	assert(near(timing(PCLK, VFP), 40.0, 0.05));
	/* The overlay: the SAME pixel clock (so the same D-PHY lane rate, DSI
	 * byte clock and line rate -- the HX8394's charge pumps run off HSYNC,
	 * SETPOWER CLK_OPT) and only a longer vertical front porch. */
	{
		long vfp  = overlay_prop("panel_30hz.overlay", "vfront-porch");
		long pclk = overlay_prop("panel_30hz.overlay", "clock-frequency");

		assert(vfp > VFP);
		assert(pclk == -1 || pclk == (long)PCLK); /* the pixel clock stays the shield's */
		assert(near(timing(PCLK, (int)vfp), 30.0, 0.05));
	}
	/* The refresh the game derives from a display's devicetree timings
	 * (panel_hz.h TR_PANEL_HZ): the real shield overlays, parsed. */
	{
		const char *rk   = "../../../zephyr/boards/shields/e1m_evk_rk055hdmipi4ma0/"
		                   "e1m_evk_rk055hdmipi4ma0.overlay";
		const char *riv  = "../../../zephyr/boards/shields/e1m_evk_rvt121hvdfwca0/"
		                   "e1m_evk_rvt121hvdfwca0.overlay";
		long        rkh  = overlay_prop(rk, "hsync-len") + overlay_prop(rk, "hback-porch") +
		                   overlay_prop(rk, "hfront-porch") + overlay_prop(rk, "width");
		long        rkv  = overlay_prop(rk, "vsync-len") + overlay_prop(rk, "vback-porch") +
		                   overlay_prop(rk, "vfront-porch") + overlay_prop(rk, "height");
		long        rkp  = overlay_prop(rk, "clock-frequency");
		long        v30  = overlay_prop("panel_30hz.overlay", "vfront-porch");
		long        rivh = overlay_prop(riv, "hsync-len") + overlay_prop(riv, "hback-porch") +
		                   overlay_prop(riv, "hfront-porch") + overlay_prop(riv, "width");
		long        rivv = overlay_prop(riv, "vsync-len") + overlay_prop(riv, "vback-porch") +
		                   overlay_prop(riv, "vfront-porch") + overlay_prop(riv, "height");
		long        rivp = overlay_prop(riv, "clock-frequency");

		assert(rkp == (long)PCLK && rkh == 762 && rkv == 1312);
		assert(tr_refresh_hz((uint32_t)rkp, (uint32_t)rkh, (uint32_t)rkv) == 40u);
		/* RK055 with panel_30hz.overlay: only the vertical front porch changes. */
		assert(tr_refresh_hz((uint32_t)rkp,
		                     (uint32_t)rkh,
		                     (uint32_t)(rkv - overlay_prop(rk, "vfront-porch") + v30)) == 30u);
		/* Riverdi RVT121: 36,363,636 Hz over 1440 x 840 = 30.06 Hz. */
		assert(rivp == 36363636L && rivh == 1440 && rivv == 840);
		assert(tr_refresh_hz((uint32_t)rivp, (uint32_t)rivh, (uint32_t)rivv) == 30u);
	}
	/* The build's nominal period is the real one (the flip histogram's bins). */
	assert(near(TR_PANEL_PERIOD_US * 1e-6, frame_s, 1e-5));
	assert(tr_flip_hist_bucket(TR_PANEL_PERIOD_US + 2000u) == 0u);
	assert(tr_flip_hist_bucket(2u * TR_PANEL_PERIOD_US) == 1u);
	{
		tr_flip_pace_t p = { 0 };

		(void)tr_flip_pace_landed(&p, 1000000u);
		assert(!tr_flip_pace_landed(&p, 1000000u + TR_PANEL_PERIOD_US * 14u / 10u));
		assert(tr_flip_pace_landed(
		    &p, 1000000u + TR_PANEL_PERIOD_US * 14u / 10u + TR_PANEL_PERIOD_US * 16u / 10u));
	}

	/* 2. Real time. Play: TR_GAME_PACE_Q8 is steps per 40 Hz frame (0.5x =
	 * 20 steps/s); the same steps per second at any refresh, never two a
	 * frame. Ten seconds of frames step 10 x 20 times (+-1). */
	{
		double steps_s = TR_PLAY_SPEED_Q16 / 65536.0 * hz;

		assert(TR_PLAY_SPEED_Q16 <= 65536u);
		assert(near(steps_s, TR_GAME_PACE_Q8 * 40.0 / 256.0, 0.01));
		assert(near(TR_ATTRACT_SPEED_Q16 / 65536.0 * hz, 0.7 * steps_s, 0.01));

		uint32_t ph = 0, steps = 0;

		for (int f = 0; f < 10 * TR_PANEL_HZ; f++) {
			steps += tr_game_pace(&ph, TR_PLAY_SPEED_Q16) ? 1u : 0u;
		}
		assert(steps >= 199u && steps <= 201u);
	}
	/* Frame-counted lengths, within half a frame of the 40 Hz build's. */
	assert(near(TR_CRASH_FRAMES * frame_s, 1.5, frame_s / 2.0));
	assert(near(TR_TILT_ENGAGE_TICKS * frame_s, 0.5, frame_s / 2.0));
	assert(near(TR_TILT_IDLE_TICKS * frame_s, 7.5, frame_s / 2.0));
	assert(near(TR_TILT_OVER_IDLE_TICKS * frame_s, 2.25, frame_s / 2.0));
	/* The HUD and the A32 animate in 40 Hz frames: a second is 40 of them. */
	assert(tr_hz_to40(0u) == 0u && tr_hz_to40((uint32_t)TR_PANEL_HZ) == 40u);
	for (uint32_t f = 0; f < 1000u; f++) {
		assert(tr_hz_to40(f + 1u) > tr_hz_to40(f));      /* never repeats a frame */
		assert(TR_PANEL_HZ != 40 || tr_hz_to40(f) == f); /* 40 Hz: unchanged */
	}
	/* The crash as the A32 sees it: every packet's crash_tick inside the
	 * renderer's 0 .. TR_CRASH_TICKS - 1, the last within a 40 Hz frame of it. */
	{
		tr_game_t     g;
		tr_frame_in_t in;
		uint32_t      ph     = 0;
		int           frames = 1;

		tr_game_init(&g, 3u);
		g.alive = false, g.crashed = true, g.crash_ticks = 0;
		while (tr_game_crash_frame(&g, &ph)) {
			frames++;
		}
		assert(frames == TR_CRASH_FRAMES);
		tr_frame_in_from_game(&in, &g, 0, false, false);
		assert(in.crash_tick <= TR_CRASH_TICKS - 1 &&
		       in.crash_tick >= TR_CRASH_TICKS - 1 - 40 / TR_PANEL_HZ);
	}

	printf("PASS: panel_hz (%d Hz)\n", TR_PANEL_HZ);
	return 0;
}
