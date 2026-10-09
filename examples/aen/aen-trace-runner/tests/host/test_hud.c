/* tests/host/test_hud.c -- the layer 2 HUD (src/hud/hud.c): screen choice,
 * number/perf text, dirty tiles, and the invariant that an incremental
 * update leaves exactly what a from-scratch paint of the same view does.
 * Prints host ns/px for the HE cost estimate. */
#define _POSIX_C_SOURCE 199309L /* clock_gettime */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../../src/game/hiscore.h"
#include "../../src/game/zone.h"
#include "../../src/hud/hud.h"
#include "../../src/ipc/tr_aring.h"
#include "../../src/ipc/tr_mbox.h"
#include "../../src/ipc/tr_memmap.h"
#ifdef TR_PARTNER_LOGO_HEADER
#include TR_PARTNER_LOGO_HEADER /* tr_partner_logo[]: the pixels hud.c was built with */
#endif

static uint16_t fb[TR_HUD_W * TR_HUD_H], ref[TR_HUD_W * TR_HUD_H];

#define MIDDLE    (1u << 3 | 1u << 4 | 1u << 5 | 1u << 6) /* hud.c T_MIDL, T_POP, T_MIDR, T_STRIP */
#define T_PWR_BIT (1u << 7)                               /* hud.c T_PWR: the power graph */
#define T_INV_BIT (1u << 8)                               /* hud.c T_INV */
#ifdef TR_PARTNER_LOGO_HEADER
/* The partner plate owns the road's left edge (x 16 .. 238, y 170 .. 259): the see-through checks
 * below start right of it. */
#define CLEAR_X(x) ((x) < 240 ? 240 : (x))
#else
#define CLEAR_X(x) (x)
#endif

static uint64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static void view_play(tr_hud_view_t *v, uint32_t score, uint32_t metres)
{
	tr_score_t s;

	tr_score_init(&s);
	s.score  = score;
	s.metres = metres;
	s.best   = 777u;
	tr_hud_view_set(v, &s, TR_BANNER_NONE, false, TR_HUD_INVITE_TILT);
	strcpy(v->perf[0], "FPS 40.0");
	v->character = 0u;
}

/* fb must equal a fresh paint of v at the hud's own frame. */
static void same_as_scratch(const tr_hud_t *h, const tr_hud_view_t *v)
{
	tr_hud_paint_all(ref,
	                 v,
	                 tr_hz_to40(h->frame - 1u),
	                 h->popup_start,
	                 h->zone_start); /* the HUD's 40 Hz clock */
	assert(memcmp(fb, ref, sizeof(fb)) == 0);
}

static uint32_t alpha_px(const uint16_t *b, int x0, int y0, int x1, int y1)
{
	uint32_t n = 0;

	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++) {
			n += (b[y * TR_HUD_W + x] >> 12) != 0u;
		}
	}
	return n;
}

int main(void)
{
	/* 1. Screen choice. */
	assert(tr_hud_mode_of(TR_BANNER_NONE, false) == TR_HUD_PLAY);
	assert(tr_hud_mode_of(TR_BANNER_ATTRACT, true) == TR_HUD_ATTRACT);
	assert(tr_hud_mode_of(TR_BANNER_GAME_OVER, true) ==
	       TR_HUD_ATTRACT); /* a demo crash keeps the card */
	assert(tr_hud_mode_of(TR_BANNER_GAME_OVER, false) == TR_HUD_CRASH);
	assert(tr_hud_mode_of(TR_BANNER_STAND, false) == TR_HUD_BANNER);
	assert(tr_hud_mode_of(TR_BANNER_STEP_BACK, false) == TR_HUD_BANNER);
	assert(tr_hud_mode_of(TR_BANNER_CHECK_CAMERA, false) == TR_HUD_BANNER);

	/* 2. Numbers. */
	char b[16];

	assert(tr_hud_fmt_u32(b, 0u) == 1 && strcmp(b, "0") == 0);
	assert(tr_hud_fmt_u32(b, 999u) == 3 && strcmp(b, "999") == 0);
	assert(tr_hud_fmt_u32(b, 1000u) == 5 && strcmp(b, "1,000") == 0);
	assert(tr_hud_fmt_u32(b, 1234567u) == 9 && strcmp(b, "1,234,567") == 0);
	assert(tr_hud_fmt_u32(b, 4294967295u) == 13 && strcmp(b, "4,294,967,295") == 0);

	/* 3. Perf panel maths + text. */
	assert(tr_perf_pct(0u, 0u) == 0u && tr_perf_pct(1u, 3u) == 33u && tr_perf_pct(2u, 3u) == 67u);
	assert(tr_perf_pct(5u, 4u) == 100u);
	{
		tr_perf_t     p   = { 0 };
		tr_hud_view_t v   = { 0 };
		tr_perf_mem_t mem = { 5883904u, 8388608u, 346112u, 180224u, 64512u };
		tr_perf_raw_t r   = { 1000000u, 100u, 0u, 0u, 0u, 0u, TR_ARING_MAGIC, 0u, 0, 0u, 0u, 0u };

		assert(!tr_perf_sample(&p, &r, &mem, &v) && v.perf[0][0] == '\0'); /* opens the window */
		r.now_us += 400000u; /* too short: no update */
		r.flips += 16u;
		assert(!tr_perf_sample(&p, &r, &mem, &v));
		r.now_us += 100000u; /* 500 ms, 20 flips, core 0 22 ms / core 1 21 ms a frame */
		r.flips += 4u;
		r.a32_ticks0  = 20u * 2200000u;
		r.a32_ticks1  = 20u * 2100000u;
		r.he_busy_cyc = 18u;
		r.he_all_cyc  = 100u;
		r.rail5v_mw   = 1234; /* platform/rail5v_power.c's EMA, mW */
		assert(tr_perf_sample(&p, &r, &mem, &v));
		assert(p.fps_x10 == 400u && p.a32_pct[0] == 88u && p.a32_pct[1] == 84u && p.he_pct == 18u);
		printf("perf: [%s] [%s] [%s] [%s] [%s] [%s]\n",
		       v.perf[0],
		       v.perf[1],
		       v.perf[2],
		       v.perf[3],
		       v.perf[4],
		       v.perf[5]);
		assert(strcmp(v.perf[0], "FPS 40.0") == 0);
		assert(strcmp(v.perf[1], "A32#0 88%  A32#1 84%") == 0);
		assert(strcmp(v.perf[2], "M55-HE 18%  M55-HP --") ==
		       0); /* ring up, no HP sound (2026W36-0009) */
		assert(strcmp(v.perf[3], "SRAM 5.61/8.00 MB") == 0);
		assert(strcmp(v.perf[4], "IMG 0.33 MB  TCM 176+63K") == 0);
		/* "5V .. mW SoM+LCD", never "SOM": U30 (R127) sits on the carrier's
		 * whole downstream +5V net -- module, display (J6 39/40), U7, U52,
		 * U6, the amps -- not an isolated module-input tap (netlist-traced,
		 * see platform/rail5v_power.c's header comment). A label reading
		 * "SOM 1234 mW" here is exactly the claim the maintainer rejected. */
		assert(strcmp(v.perf[5], "5V 1234 mW SoM+LCD") == 0);
		/* The HP holds I2C2 for its amp bring-up (platform/bus2_he.h): the reading is stale and
		 * the line says so ("--"), not a frozen number; 0 mW is a real reading, not "--". */
		r.now_us += 500000u;
		r.flips += 20u;
		r.rail5v_mw = -1;
		assert(tr_perf_sample(&p, &r, &mem, &v) && strcmp(v.perf[5], "5V -- mW SoM+LCD") == 0);
		r.now_us += 500000u;
		r.flips += 20u;
		r.rail5v_mw = 0;
		assert(tr_perf_sample(&p, &r, &mem, &v) && strcmp(v.perf[5], "5V 0 mW SoM+LCD") == 0);
		r.now_us += 500000u;
		r.flips += 20u;
		r.rail5v_mw = 1234;
		assert(tr_perf_sample(&p, &r, &mem, &v));
		/* 39 flips in 1.000 s after a hold: 39.0, not a stale 40. */
		r.now_us += 1000000u;
		r.flips += 39u;
		assert(tr_perf_sample(&p, &r, &mem, &v) && strcmp(v.perf[0], "FPS 39.0") == 0);
		assert(strcmp(v.perf[1], "A32#0 0%  A32#1 0%") ==
		       0); /* no frame landed ticks this window */
		/* The HP word follows the sound ring's status, never a fixed "idle". */
		r.now_us += 500000u;
		r.hp_state = 1u;
		assert(tr_perf_sample(&p, &r, &mem, &v) &&
		       strcmp(v.perf[2], "M55-HE 0%  M55-HP audio") == 0);
		assert(strcmp(tr_perf_hp(0u, 0u), "--") == 0 &&
		       strcmp(tr_perf_hp(0xFFFFFFFFu, 1u), "--") == 0);
		assert(strcmp(tr_perf_hp(TR_ARING_MAGIC, TR_ARING_HP_OFF), "--") == 0);
		assert(strcmp(tr_perf_hp(TR_ARING_MAGIC, TR_ARING_HP_RUNNING), "audio") == 0);
		assert(strcmp(tr_perf_hp(TR_ARING_MAGIC, TR_ARING_HP_FAULT), "fault") == 0);
		assert(strcmp(tr_perf_hp(TR_ARING_MAGIC, 7u), "?") == 0);
	}

	/* 3a2 (fix round 5): hp_vision's own beacon (src/ipc/tr_hp_dbg.h), when
	 * its magic is valid on BOTH ends of a window, shows a real busy % from
	 * its cumulative cycle counters, not the sound ring's text. Deliberately
	 * NOT the same tr_perf_t/tr_perf_raw_t as 3a above -- p->have must open
	 * fresh so this window's magic really is "valid at both ends", not
	 * carrying over 3a's already-closed sound-ring window. */
	{
		tr_perf_t     p   = { 0 };
		tr_hud_view_t v   = { 0 };
		tr_perf_mem_t mem = { 5883904u, 8388608u, 346112u, 180224u, 64512u };
		tr_perf_raw_t r   = { 0 };

		r.now_us       = 1000000u;
		r.hp_dbg_magic = TR_HP_DBG_MAGIC;
		r.hp_busy_cyc  = 1000u;
		r.hp_total_cyc = 2000u;
		assert(!tr_perf_sample(&p, &r, &mem, &v)); /* opens the window */
		r.now_us += 500000u;                       /* 500 ms window, closes it */
		r.hp_busy_cyc += 30u;                      /* 30 of 100 cycles this window: 30% */
		r.hp_total_cyc += 100u;
		assert(tr_perf_sample(&p, &r, &mem, &v));
		assert(p.hp_pct == 30u);
		assert(strcmp(v.perf[2], "M55-HE 0%  M55-HP 30%") == 0);

		/* Magic drops (a reset, or no hp_vision resident this boot): falls
		 * back to the sound-ring text, same as if hp_dbg had never been
		 * valid -- proves the fallback is live, not just the initial state. */
		r.now_us += 500000u;
		r.hp_dbg_magic = 0u;
		r.hp_state     = TR_ARING_HP_RUNNING;
		r.hp_magic     = TR_ARING_MAGIC;
		assert(tr_perf_sample(&p, &r, &mem, &v));
		assert(strcmp(v.perf[2], "M55-HE 0%  M55-HP audio") == 0);
	}

	/* 3a3 (fix round 7): the window where the HP's beacon FIRST becomes
	 * valid -- magic seen this sample, but the PREVIOUS sample (p->last)
	 * predates it -- must show "--", not a misleading "0%" indistinguishable
	 * from a genuinely idle HP (the exact ambiguity the silicon finding
	 * turned out to hinge on: a real % needs two valid-magic samples, and
	 * showing 0 for the one window that has only one is a lie by omission). */
	{
		tr_perf_t     p   = { 0 };
		tr_hud_view_t v   = { 0 };
		tr_perf_mem_t mem = { 5883904u, 8388608u, 346112u, 180224u, 64512u };
		tr_perf_raw_t r   = { 0 };

		r.now_us = 1000000u; /* magic NOT valid yet -- opens the window */
		assert(!tr_perf_sample(&p, &r, &mem, &v));

		r.now_us += 500000u; /* the HP boots mid-window: magic valid NOW, but
				       * p->last (above) was not -- no real delta yet */
		r.hp_dbg_magic = TR_HP_DBG_MAGIC;
		r.hp_busy_cyc  = 500000000u; /* an arbitrary large cumulative value,
					      * NOT the tiny delta from an invalid
					      * baseline -- proves this is skipped
					      * on the magic gate, not by coincidence */
		r.hp_total_cyc = 500000000u;
		assert(tr_perf_sample(&p, &r, &mem, &v));
		assert(!p.hp_pct_valid);
		assert(strcmp(v.perf[2], "M55-HE 0%  M55-HP --") == 0);

		r.now_us += 500000u; /* both ends of THIS window are valid: a real % */
		r.hp_busy_cyc += 60u;
		r.hp_total_cyc += 100u;
		assert(tr_perf_sample(&p, &r, &mem, &v));
		assert(p.hp_pct_valid && p.hp_pct == 60u);
		assert(strcmp(v.perf[2], "M55-HE 0%  M55-HP 60%") == 0);
	}

	/* 3b. The SRAM allocation map: inside SRAM0 + SRAM1, no two regions
	 * overlap, the HUD buffer clear of FB A, the MHU0 window and the stub's
	 * early park page; the renderer counted by its real end when known. */
	{
		tr_mem_region_t m[TR_MEM_REGIONS];
		unsigned        n = tr_mem_map(m, 0u);

		for (unsigned i = 0; i < n; i++) {
			assert(m[i].size > 0u && m[i].base >= TR_MEM_SRAM_BASE &&
			       m[i].base + m[i].size <= TR_MEM_SRAM_BASE + TR_MEM_SRAM_TOTAL);
			for (unsigned j = i + 1u; j < n; j++) {
				assert(m[i].base + m[i].size <= m[j].base || m[j].base + m[j].size <= m[i].base);
			}
		}
		assert(TR_HUD_FB >= TR_FB_A + TR_FB_SLOT_SIZE && TR_HUD_FB >= TR_MHU0_WINDOW_HI &&
		       TR_HUD_FB + TR_HUD_FB_SIZE <= 0x023FE000u);
		/* The renderer's buffers are counted where render.c puts them
		 * (both use tr_memmap.h), and the P10 ring + the stub's park page
		 * are in the map, so the overlap check above covers them. */
		static const struct {
			const char *name;
			uint32_t    base;
		} want[] = { { "A32 setup", TR_MEM_A32_SETUP },
			         { "A32 bands", TR_MEM_A32_BANDS },
			         { "A32 DL1", TR_MEM_A32_DL1 },
			         { "sound ring", TR_MEM_ARING },
			         { "stub park", 0x023FE000u },
			         { "A32 DL", TR_MEM_A32_DL },
			         { "A32 bins", TR_MEM_A32_BINS },
			         { "A32 stacks", TR_MEM_A32_STACKS },
			         { "camera pool", TR_MEM_CAM_POOL },
			         { "A32 gate", TR_MEM_A32_GATE },
			         { "FB B", TR_FB_B },
			         { "HUD", TR_HUD_FB },
			         { "A32 zone tex", TR_MEM_A32_ZTEX },
			         { "A32 zone idx", TR_MEM_A32_ZIDX } };

		for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
			unsigned i = 0;

			while (i < n && strcmp(m[i].name, want[k].name) != 0) {
				i++;
			}
			assert(i < n && m[i].base == want[k].base);
		}
		uint32_t budget = tr_mem_sram_used(0u), real = tr_mem_sram_used(0x02537670u);

		assert(budget - real ==
		       0xC0000u - 0x37670u); /* image + .bss may run to TR_MEM_A32_IMG_END */
		assert(tr_mem_sram_used(0x02600000u) == budget); /* out of range: the budget */
		printf("mem: SRAM allocated %u B (renderer budget) / %u B (renderer 0x37670), of %u\n",
		       (unsigned)budget,
		       (unsigned)real,
		       TR_MEM_SRAM_TOTAL);
	}

	/* 4. Every tagline / prompt character has a glyph. */
	assert(tr_hud_text_w(TR_HUD_FONT_SMALL,
	                     "E1M-AEN803 \x7f Alif Ensemble E8 \x7f 2x Cortex-A32 \x7f 3D at 40 fps") <=
	       TR_HUD_W - 24);
	assert(tr_hud_text_w(TR_HUD_FONT_MED, "STEP BACK INTO VIEW") <= TR_HUD_W - 40);
	assert(tr_hud_text_w(TR_HUD_FONT_BIG, "4,294,967,295") > tr_hud_text_w(TR_HUD_FONT_BIG, "1"));

	/* 5. Dirty tiles: the first update paints everything; an unchanged
	 * view paints nothing; a score change repaints only the score tile. */
	tr_hud_t      h;
	tr_hud_view_t v;
	uint32_t      dirty, px;

	memset(fb, 0xA5, sizeof(fb)); /* whatever SRAM0 held */
	tr_hud_init(&h);
	view_play(&v, 0u, 0u);
	px = tr_hud_update(&h, fb, &v, &dirty);
	assert(px >= (uint32_t)TR_HUD_W * TR_HUD_H && dirty == (1u << TR_HUD_TILES) - 1u);
	same_as_scratch(&h, &v);
	assert(tr_hud_update(&h, fb, &v, &dirty) == 0u && dirty == 0u);
	/* Play: the score block is drawn, the centre stays see-through. */
	assert(alpha_px(fb, 0, 0, 360, 150) > 2000u);
	assert(alpha_px(fb, CLEAR_X(200), 180, 520, 340) == 0u);

	view_play(&v, 555u, 0u); /* under BEST (777): only the score tile moves */
	px = tr_hud_update(&h, fb, &v, &dirty);
	assert(dirty == 1u && px > 0u && px < (uint32_t)TR_HUD_W * TR_HUD_H / 4u);
	same_as_scratch(&h, &v);
	/* P16: the character's name is in the score tile */
	v.character = 3u;
	px          = tr_hud_update(&h, fb, &v, &dirty);
	assert(dirty == 1u && px > 0u);
	same_as_scratch(&h, &v);
	assert(strcmp(tr_hud_char_name(3u), "PIXEL") == 0 &&
	       strcmp(tr_hud_char_name(9u), "PROBE") == 0);
	v.character = 0u;
	(void)tr_hud_update(&h, fb, &v, &dirty);

	uint64_t t0 = now_ns();
	uint32_t n  = 0;

	/* 6. A pickup popup: rises and fades for TR_HUD_POPUP_FRAMES frames of
	 * the HUD's 40 Hz clock (one update each at 40 Hz), then clears once;
	 * every frame equals a scratch paint. */
	v.popup_seq  = 1u;
	v.popup_pts  = 20u;
	v.popup_mult = 2u;
	v.combo      = 2u;
	for (uint32_t f = 0, prev = 0; f < TR_HUD_POPUP_FRAMES + 3u; f++) {
		px = tr_hud_update(&h, fb, &v, &dirty);
		n += px;
		same_as_scratch(&h, &v);
		uint32_t age = tr_hz_to40(h.frame - 1u) - h.popup_start;

		if (f == 0u) {
			assert(alpha_px(fb, 200, 150, 520, 300) > 500u);
		}
		if (f == 0u || age < TR_HUD_POPUP_FRAMES || prev < TR_HUD_POPUP_FRAMES) {
			assert(px > 0u); /* showing, or the clear right after */
		} else {
			assert(px == 0u);
		}
		prev = age;
	}
	assert(alpha_px(fb, CLEAR_X(200), 180, 520, 298) == 0u);

	/* 6b. A zone entry (P15): its name shows in the row under the centre for
	 * TR_HUD_ZONE_FRAMES of the 40 Hz clock, only that row's tile repaints,
	 * then it clears once; on the attract card it takes the invitation's
	 * place for the same time. Every frame equals a scratch paint, and
	 * differs from one without the zone's popup exactly while it shows. */
	for (int attract = 0; attract < 2; attract++) {
		static uint16_t none[TR_HUD_W * TR_HUD_H];
		tr_score_t      zs;
		uint32_t        seen = 0, quiet = 0, repaints = 0;

		tr_score_init(&zs);
		zs.popup_seq = h.popup_seq; /* no pickup popup over it */
		tr_hud_view_set(
		    &v, &zs, attract ? TR_BANNER_ATTRACT : TR_BANNER_NONE, attract, TR_HUD_INVITE_NONE);
		tr_hud_view_zone(&v, TR_ZONE_MEM, 7u + (uint32_t)attract);
		for (uint32_t f = 0; f < TR_HUD_ZONE_FRAMES + 4u; f++) {
			px = tr_hud_update(&h, fb, &v, &dirty);
			same_as_scratch(&h, &v);
			uint32_t fr = tr_hz_to40(h.frame - 1u), age = fr - h.zone_start;
			int      rows;

			tr_hud_paint_all(none, &v, fr, h.popup_start, h.zone_start - TR_HUD_ZONE_FRAMES);
			rows = memcmp(&fb[300 * TR_HUD_W],
			              &none[300 * TR_HUD_W],
			              (TR_HUD_H - 300) * TR_HUD_W * 2) != 0;
			assert(memcmp(fb, none, 300 * TR_HUD_W * 2) == 0); /* nothing above the row */
			if (age < TR_HUD_ZONE_FRAMES) {
				assert(rows);
				seen++;
				assert(f == 0u || (dirty & ~T_INV_BIT) == 0u); /* T_INV only */
				repaints += (dirty >> 6) & 1u;
			} else {
				assert(!rows);
				quiet++;
			}
		}
		assert(seen + 1u >= TR_HZ_FRAMES(TR_HUD_ZONE_FRAMES) &&
		       quiet >= 3u); /* its real time at 30 Hz too */
		/* keyed on the name's alpha, not its age: the row (37,440 px)
		 * repaints while it fades in (8) and out (16), not every frame */
		assert(repaints <= 8u + 16u + 2u);
		printf("hud: zone popup (%s) %u frames, %u row repaints\n",
		       attract ? "attract" : "play",
		       (unsigned)seen,
		       (unsigned)repaints);
	}
	/* The attract row, P16 + P15 (hud.c INV_CHAR_X 190 / INV_SIDE_X 520):
	 * the character's name and its tilt arrows (34 px past the name) stay
	 * clear of any zone name or invitation beside them, and inside the card. */
	for (uint8_t c = 0; c < TR_CHAR_N; c++) {
		int wc = tr_hud_text_w(TR_HUD_FONT_MED, tr_hud_char_name(c));

		assert(190 - wc / 2 - 34 >= 20);
		for (uint32_t z = 0; z < TR_ZONES + 2u; z++) {
			const char *side = z < TR_ZONES    ? tr_zone_name(z)
			                   : z == TR_ZONES ? "TILT TO PLAY"
			                                   : "STEP IN TO PLAY";
			int         ws   = tr_hud_text_w(TR_HUD_FONT_MED, side);

			assert(190 + wc / 2 + 34 < 520 - ws / 2 && 520 + ws / 2 <= TR_HUD_W - 20);
		}
	}

	/* 6c. Booth: the high-score table rotates with the logo on the attract
	 * card, a page every TR_HUD_PAGE_FRAMES of the 40 Hz clock, in the
	 * card's middle only -- the row under it (character + invitation, or a
	 * zone's name) is the same on either page, and a zone's name changes
	 * only that row on the table page: the two never overlap. No table:
	 * no table page. */
	{
		static uint16_t a[TR_HUD_W * TR_HUD_H], b[TR_HUD_W * TR_HUD_H], c[TR_HUD_W * TR_HUD_H];
		tr_hiscore_t    hs;
		tr_score_t      ts;
		const size_t    row = TR_HUD_W * 2u;
		const uint32_t  P = TR_HUD_PAGE_FRAMES, off = 0u - TR_HUD_ZONE_FRAMES;

		assert(P % TR_HUD_BLINK_FRAMES == 0u); /* the invitation blinks the same on both pages */
		tr_score_init(&ts);
		tr_hs_init(&hs);
		tr_hud_view_set(&v, &ts, TR_BANNER_ATTRACT, true, TR_HUD_INVITE_TILT);
		tr_hud_view_booth(&v, &hs, NULL);
		tr_hud_paint_all(a, &v, 0u, 0u - TR_HUD_POPUP_FRAMES, off);
		tr_hud_paint_all(b, &v, P, 0u - TR_HUD_POPUP_FRAMES, P + off);
		assert(memcmp(a, b, sizeof(a)) == 0); /* empty table: the logo stays */
		(void)tr_hs_insert(&hs, 15230u, "ACE");
		(void)tr_hs_insert(&hs, 12480u, "E8 ");
		(void)tr_hs_insert(&hs, 9001u, "PRO");
		(void)tr_hs_insert(&hs, 8645u, "SOL");
		(void)tr_hs_insert(&hs, 120u, "W W");
		tr_hud_view_booth(&v, &hs, NULL);
		assert(v.mode == TR_HUD_ATTRACT);
		for (uint32_t k = 0; k < 4u; k++) {
			uint32_t fl = 2u * k * P, ft = fl + P; /* a logo page, then a table page */

			tr_hud_paint_all(a, &v, fl, 0u - TR_HUD_POPUP_FRAMES, fl + off);
			tr_hud_paint_all(b, &v, ft, 0u - TR_HUD_POPUP_FRAMES, ft + off);
			assert(memcmp(a, b, 140 * row) == 0); /* above the card: untouched */
			assert(memcmp(a + 140 * TR_HUD_W, b + 140 * TR_HUD_W, 160 * row) !=
			       0); /* the card's middle turns */
			assert(memcmp(a + 300 * TR_HUD_W, b + 300 * TR_HUD_W, 52 * row) ==
			       0); /* the invitation row: not */
			assert(alpha_px(b, 60, 150, 660, 296) > 8000u);
			/* the zone's name over the table page: only its row changes */
			tr_hud_paint_all(c, &v, ft, 0u - TR_HUD_POPUP_FRAMES, ft);
			assert(memcmp(b, c, 300 * row) == 0);
			assert(memcmp(b + 300 * TR_HUD_W, c + 300 * TR_HUD_W, 52 * row) != 0);
		}
		/* incremental == scratch across page turns */
		tr_hud_t hp;
		uint32_t worst = 0, turns = 0;

		tr_hud_init(&hp);
		for (uint32_t f = 0; f < TR_HZ_FRAMES(3u * P); f++) {
			px = tr_hud_update(&hp, fb, &v, &dirty);
			same_as_scratch(&hp, &v);
			if (f > 0u && (dirty & (1u << 4))) {
				turns++;
				worst = px > worst ? px : worst;
			}
		}
		/* a turn: the card's three middle tiles (720 x 160), plus the
		 * invitation row when its blink lands on the same frame; the HE's
		 * cap spreads that over two frames (case 8) */
		assert(turns >= 2u && worst <= 720u * 160u + 720u * 52u);
		printf("hud: high-score page turn repaints %u px\n", (unsigned)worst);
		/* with the HE's cap (hud_l2.c HUD_PX_BUDGET 110,000) a turn is never split: every
		 * frame's card middle -- the three middle tiles AND the tagline strip's end (T_STRIP) --
		 * is wholly one page or the other. 100,000 is the margin case: the group (97.6 k px)
		 * fits it and the strip behind it (3 k) would not, were it not part of the group. */
		for (int bi = 0; bi < 2; bi++) {
			static uint16_t lp[TR_HUD_W * TR_HUD_H], tp[TR_HUD_W * TR_HUD_H];
			tr_hud_t        hb;
			uint32_t        seen_t = 0;
			const size_t    mrow   = 160u * TR_HUD_W * 2u;

			tr_hud_paint_all(lp, &v, 0u, 0u - TR_HUD_POPUP_FRAMES, off);
			tr_hud_paint_all(tp, &v, P, 0u - TR_HUD_POPUP_FRAMES, P + off);
			tr_hud_init(&hb);
			hb.budget = bi ? 100000u : 110000u;
			for (uint32_t f = 0; f < TR_HZ_FRAMES(3u * P); f++) {
				px = tr_hud_update(&hb, fb, &v, &dirty);
				if (f < 3u) {
					continue; /* the first, whole-screen paint spreads over frames */
				}
				bool is_l = memcmp(fb + 140 * TR_HUD_W, lp + 140 * TR_HUD_W, mrow) == 0;
				bool is_t = memcmp(fb + 140 * TR_HUD_W, tp + 140 * TR_HUD_W, mrow) == 0;

				assert(is_l || is_t);
				seen_t += is_t;
				assert(px <= 720u * 160u || (dirty & ~MIDDLE) == 0u);
			}
			assert(seen_t > 0u);
		}
		/* the widest row fits the card */
		assert(tr_hud_text_w(TR_HUD_FONT_MED, "5  WWW  4,294,967,295") <= TR_HUD_W - 2 * 60);
	}

	/* 6d. Booth: initials entry. Its own screen (the score panel + a card:
	 * NEW HIGH SCORE, the place, the three letters); cycling a letter
	 * repaints only the letters' tile (T_POP), well inside the HE's
	 * repaint cap, and always equals a scratch paint. */
	{
		tr_hiscore_t  hs;
		tr_initials_t e;
		tr_score_t    ts;
		tr_hud_t      hi;
		uint32_t      worst = 0;

		tr_score_init(&ts);
		ts.score = 15230u;
		tr_hs_init(&hs);
		(void)tr_hs_insert(&hs, 20000u, "TOP");
		tr_ini_start(&e, "PRO", 1);
		tr_hud_init(&hi);
		for (uint32_t f = 0; f < 200u; f++) {
			tr_intent_t in = tr_intent_none();

			in.lane_delta = (int8_t)(f % 17u == 3u ? 1 : f % 23u == 5u ? -1 : 0);
			in.jump       = f == 120u || f == 160u;
			(void)tr_ini_step(&e, in);
			tr_hud_view_set(&v, &ts, TR_BANNER_NONE, false, TR_HUD_INVITE_TILT);
			tr_hud_view_booth(&v, &hs, &e);
			assert(v.mode == TR_HUD_INITIALS);
			px = tr_hud_update(&hi, fb, &v, &dirty);
			same_as_scratch(&hi, &v);
			if (f > 2u) {
				assert((dirty & ~(1u << 4)) == 0u); /* the letters' tile only */
				worst = px > worst ? px : worst;
			}
		}
		assert(worst > 0u && worst <= 110000u / 2u);
		assert(tr_hud_text_w(TR_HUD_FONT_SMALL,
		                     "TILT \x7f LETTER   TOWARD \x7f NEXT   AWAY \x7f BACK") <=
		       TR_HUD_W - 220 - 2 * 16); /* inside the card (110 .. 610), clear of its edge rule */
		assert(alpha_px(fb, 230, 190, 490, 290) > 3000u); /* the letters are up */
		printf("hud: initials entry repaints <= %u px a frame\n", (unsigned)worst);
	}

	/* 6e. Booth popups: the combo multiplier rides the points popup (x2
	 * to x5), and the new-high-score celebration is a popup of its own --
	 * both inside the popup's tile, so nothing else repaints for them. */
	{
		static uint16_t a[TR_HUD_W * TR_HUD_H], b[TR_HUD_W * TR_HUD_H];
		tr_score_t      ts;

		tr_score_init(&ts);
		ts.popup_seq = 1u;
		for (int hsp = 0; hsp < 2; hsp++) {
			ts.popup_hs   = (uint8_t)hsp;
			ts.popup_pts  = hsp ? 0u : 50u;
			ts.popup_mult = hsp ? 0u : 5u;
			ts.combo      = hsp ? 0u : 5u;
			tr_hud_view_set(&v, &ts, TR_BANNER_NONE, false, TR_HUD_INVITE_TILT);
			tr_hud_paint_all(a, &v, 0u, 0u, 0u - TR_HUD_ZONE_FRAMES);
			tr_hud_paint_all(b, &v, 0u, 0u - TR_HUD_POPUP_FRAMES, 0u - TR_HUD_ZONE_FRAMES);
			for (int y = 0; y < TR_HUD_H; y++) {
				for (int x = 0; x < TR_HUD_W; x++) {
					bool in_pop = y >= 140 && y < 300 && x >= 220 && x < 500;

					assert(in_pop || a[y * TR_HUD_W + x] == b[y * TR_HUD_W + x]);
				}
			}
			assert(alpha_px(a, 220, 140, 500, 300) > alpha_px(b, 220, 140, 500, 300) + 1500u);
		}
	}

	/* 6b. The power graph (the +5V net, T_PWR): readouts, scale, gaps, the tile, the layout. */
	{
		int16_t w[TR_PWR_N];
		int32_t now, avg, peak;

		/* the maths, by hand: a gap is not a zero */
		for (int i = 0; i < TR_PWR_N; i++) {
			w[i] = TR_PWR_GAP;
		}
		assert(tr_hud_pwr_stats(w, &now, &avg, &peak) == 0 && now == -1 && avg == -1 && peak == -1);
		int32_t lo, span;

		tr_hud_pwr_scale(w, &lo, &span);
		assert(lo == 0 && span == TR_PWR_MIN_SPAN_MW); /* no data: the minimum span */
		w[TR_PWR_N - 1] = 2000;
		w[TR_PWR_N - 2] = 2210;
		w[TR_PWR_N - 4] = 0; /* a real zero counts: avg of 2000, 2210, 0 */
		assert(tr_hud_pwr_stats(w, &now, &avg, &peak) == 3 && now == 2000 && peak == 2210 &&
		       avg == 1403);
		w[TR_PWR_N - 1] = TR_PWR_GAP; /* the newest is a gap: "now --", the rest stands */
		assert(tr_hud_pwr_stats(w, &now, &avg, &peak) == 2 && now == -1 && avg == 1105 &&
		       peak == 2210);
		/* the scale is tight on the data: 0..2210 -> span 2762.5 up to 2800, centred 1105 - 1400 < 0 -> 0 */
		tr_hud_pwr_scale(w, &lo, &span);
		assert(lo == 0 && span == 2800 && span >= 2210 - 0);
		/* the real case: 4.85 W with ~50 mW swings fills the box, it does not sit in a 5000 mW one */
		for (int i = 0; i < TR_PWR_N; i++) {
			w[i] = (int16_t)(4825 + (i % 2) * 50);
		}
		tr_hud_pwr_scale(w, &lo, &span);
		assert(span == 100 &&
		       lo == 4800); /* range 50 x 1.25 = 62.5 -> the 100 minimum; centre 4850 */
		for (int i = 0; i < TR_PWR_N; i++) {
			assert(w[i] >= lo && w[i] <= lo + span); /* every sample inside */
			w[i] = TR_PWR_GAP;
		}
		w[3] = 4000;
		w[4] = 4400; /* range 400 x 1.25 = 500; centre 4200 - 250 = 3950 */
		tr_hud_pwr_scale(w, &lo, &span);
		assert(span == 500 && lo == 3950 && lo % TR_PWR_STEP_MW == 0 && span % TR_PWR_STEP_MW == 0);
		assert(4000 >= lo && 4400 <= lo + span);
		w[4] = 4390; /* flooring the bottom must never push the top out: the peak stays inside */
		tr_hud_pwr_scale(w, &lo, &span);
		assert(4390 <= lo + span && 4000 >= lo && lo % TR_PWR_STEP_MW == 0);
		for (int i = 0; i < TR_PWR_N; i++) {
			w[i] = TR_PWR_GAP;
		}
		w[TR_PWR_N - 1] = 40; /* near zero: the bottom is clamped at 0 */
		tr_hud_pwr_scale(w, &lo, &span);
		assert(lo == 0 && span == 100 && 40 <= span);

		/* the readouts fit the panel's 100 px at the worst int16 width, and so do the titles */
		assert(tr_hud_text_w(TR_HUD_FONT_TINY, "+5V net") <=
		       92); /* PWR_W 100 less a 4 px inset each side */
		assert(tr_hud_text_w(TR_HUD_FONT_TINY, "(SoM+LCD)") <=
		       92); /* PWR_W 100 less a 4 px inset each side */
		assert(tr_hud_text_w(TR_HUD_FONT_TINY, "now 32767") <=
		       92); /* PWR_W 100 less a 4 px inset each side */
		assert(tr_hud_text_w(TR_HUD_FONT_TINY, "avg 32767") <=
		       92); /* PWR_W 100 less a 4 px inset each side */
		assert(tr_hud_text_w(TR_HUD_FONT_TINY, "pk 32767") <=
		       92); /* PWR_W 100 less a 4 px inset each side */

		/* a sample sequence with gaps (the HP holding I2C2 for a stretch): the tile alone moves,
		 * and every update equals a scratch paint */
		tr_hud_t      hp;
		tr_hud_view_t pv;
		uint32_t      d2;

		tr_hud_init(&hp);
		view_play(&pv, 0u, 0u);
		memset(fb, 0xA5, sizeof(fb));
		(void)tr_hud_update(&hp, fb, &pv, &d2);
		same_as_scratch(&hp, &pv);
		for (uint32_t n = 0; n < 250u; n++) {
			int16_t sample = (n / 30u) % 3u == 1u ? (int16_t)TR_PWR_GAP
			                                      : (int16_t)(1800 + (int)(n * 37u % 900u));

			/* oldest first: shift the window and append, as the platform ring reads out */
			memmove(pv.pwr, pv.pwr + 1, (TR_PWR_N - 1) * sizeof(pv.pwr[0]));
			pv.pwr[TR_PWR_N - 1] = sample;
			pv.pwr_seq++;
			px = tr_hud_update(&hp, fb, &pv, &d2);
			assert(d2 == T_PWR_BIT && px > 0u && px <= 110u * 132u);
			same_as_scratch(&hp, &pv);
		}
		/* unchanged: nothing repaints */
		assert(tr_hud_update(&hp, fb, &pv, &d2) == 0u && d2 == 0u);
		/* a sample window with a gap in it paints a hole, not a bar: the gap's column keeps the
		 * panel's own pixels (as with no samples at all), its neighbours carry the bar */
		{
			static uint16_t bare[TR_HUD_W * TR_HUD_H];
			tr_hud_view_t   g = pv;
			const int       y = 202 + 24; /* mid-graph */

			g.pwr_seq = 0u; /* nothing sampled: the empty panel */
			tr_hud_paint_all(bare, &g, 0u, 0u, 0u);
			for (int i = 0; i < TR_PWR_N; i++) {
				g.pwr[i] = 1000;
			}
			g.pwr[40] = TR_PWR_GAP;
			g.pwr_seq = 99u;
			tr_hud_paint_all(ref, &g, 0u, 0u, 0u);
			assert(ref[y * TR_HUD_W + 614 + 40] == bare[y * TR_HUD_W + 614 + 40]);
			assert(ref[y * TR_HUD_W + 614 + 39] != bare[y * TR_HUD_W + 614 + 39]);
			assert(ref[y * TR_HUD_W + 614 + 41] != bare[y * TR_HUD_W + 614 + 41]);
			/* and a real zero is a bar of one pixel, not a hole */
			g.pwr[40] = 0;
			tr_hud_paint_all(ref, &g, 0u, 0u, 0u);
			assert(ref[(202 + 47) * TR_HUD_W + 614 + 40] != bare[(202 + 47) * TR_HUD_W + 614 + 40]);
		}

		/* The graph is a LINE, tightly scaled: the rail really sits at ~4.85 W with ~50 mW swings, so a
		 * filled area on a 500 mW minimum span was a flat solid block. A flat series draws a thin line,
		 * not a block (few pixels per column), and a +-25 mW wobble reaches many distinct rows. */
		{
			static uint16_t bare[TR_HUD_W * TR_HUD_H];
			tr_hud_view_t   g = pv;
			enum {
				GX = 614,
				GY = 202,
				GH = 48
			}; /* the graph box: hud.c PWR_GX / PWR_GY / PWR_GH */
			int worst_col = 0, rows = 0;
			int row_hit[GH] = { 0 };

			g.pwr_seq = 0u;
			tr_hud_paint_all(bare, &g, 0u, 0u, 0u);

			/* a flat 4850 mW */
			for (int i = 0; i < TR_PWR_N; i++) {
				g.pwr[i] = 4850;
			}
			g.pwr_seq = 500u;
			tr_hud_paint_all(ref, &g, 0u, 0u, 0u);
			for (int x = GX; x < GX + TR_PWR_N; x++) {
				int n = 0;

				for (int y = GY; y < GY + GH; y++) {
					n += ref[y * TR_HUD_W + x] != bare[y * TR_HUD_W + x];
				}
				worst_col = n > worst_col ? n : worst_col;
				assert(n >= 1); /* and it is there in every column */
			}
			assert(worst_col <= 3); /* a line, not a filled block */

			/* a +-25 mW wobble around 4850 mW, 12 samples a period */
			static const int16_t wob[12] = { 0, 12, 22, 25, 22, 12, 0, -12, -22, -25, -22, -12 };

			for (int i = 0; i < TR_PWR_N; i++) {
				g.pwr[i] = (int16_t)(4850 + wob[i % 12]);
			}
			g.pwr_seq = 501u;
			tr_hud_paint_all(ref, &g, 0u, 0u, 0u);
			worst_col = 0;
			for (int x = GX; x < GX + TR_PWR_N; x++) {
				int n = 0;

				for (int y = GY; y < GY + GH; y++) {
					if (ref[y * TR_HUD_W + x] != bare[y * TR_HUD_W + x]) {
						n++;
						row_hit[y - GY] = 1;
					}
				}
				worst_col = n > worst_col ? n : worst_col;
			}
			for (int r = 0; r < GH; r++) {
				rows += row_hit[r];
			}
			assert(rows >= 20); /* the swing is visible: it spans many rows of the box */
			assert(worst_col <=
			       12); /* ... as a connected line (one step between neighbours), not a fill */
		}

		/* the layout: the cards end where the power column begins, in EVERY screen -- the
		 * margins around the panel (x 610..612 and 712..720, rows 168..170 and 298..300) stay
		 * transparent, and the quiet screens leave the panel's own rows to it */
		for (uint8_t mode = TR_HUD_PLAY; mode <= TR_HUD_INITIALS; mode++) {
			tr_hud_view_t m;

			view_play(&m, 1234u, 56u);
			m.mode     = mode;
			m.banner   = TR_BANNER_STAND;
			m.best     = 99999u;
			m.new_best = 1;
			m.hs.n     = 5;
			for (int i = 0; i < m.hs.n; i++) {
				m.hs.e[i].score = 4000u - 100u * (uint32_t)i;
				memcpy(m.hs.e[i].name, "ABC", 4);
			}
			tr_hud_paint_all(ref, &m, 250u, 0u, 0u); /* a table page for the attract card */
			assert(alpha_px(ref, 610, 140, 612, 300) == 0u);
			assert(alpha_px(ref, 712, 140, TR_HUD_W, 300) == 0u);
			assert(alpha_px(ref, 612, 168, 712, 170) == 0u);
			assert(alpha_px(ref, 612, 298, 712, 300) == 0u);
			assert(alpha_px(ref, 612, 170, 712, 298) > 8000u); /* the panel itself is there */
		}
		printf("hud power: readouts, scale, gaps, tile, layout ok\n");
	}

	/* 7. Screen changes + a scripted mix: always equal to scratch. */
	static const uint8_t banners[] = {
		TR_BANNER_NONE,  TR_BANNER_ATTRACT,   TR_BANNER_GAME_OVER,
		TR_BANNER_STAND, TR_BANNER_STEP_BACK, TR_BANNER_CHECK_CAMERA
	};
	tr_score_t s;

	tr_score_init(&s);
	for (uint32_t f = 0; f < 400u; f++) {
		uint8_t ban  = banners[(f / 37u) % 6u];
		bool attract = ban == TR_BANNER_ATTRACT || (ban == TR_BANNER_GAME_OVER && (f / 222u) % 2u);

		s.score    = f * 3u;
		s.metres   = f / 4u;
		s.best     = 500u;
		s.combo    = (uint8_t)(f / 50u % 4u);
		s.new_best = (uint8_t)(f / 100u % 2u);
		if (f % 90u == 5u) {
			s.popup_seq++;
			s.popup_pts  = 30u;
			s.popup_mult = 3u;
		}
		tr_hud_view_set(&v, &s, ban, attract, (uint8_t)(f / 150u % 3u));
		v.character = (uint8_t)(f / 29u % 5u); /* P16, 4: out of range */
		tr_hud_view_zone(
		    &v, (uint8_t)(f / 60u % TR_ZONES), f / 60u); /* a zone entry every 60 frames */
		if (f % 20u == 0u) {
			snprintf(v.perf[0], TR_PERF_COLS, "FPS %u.0", (unsigned)(30u + f % 11u));
		}
		n += tr_hud_update(&h, fb, &v, &dirty);
		same_as_scratch(&h, &v);
		if (f == 40u) {
			/* Attract: the logo card (under BEST) + invitation are up, and
			 * the half layout's road and runner stay clear (polish round:
			 * the old card covered rows 140..348 edge to edge). */
			assert(v.mode == TR_HUD_ATTRACT && alpha_px(fb, 16, 74, 388, 138) > 20000u);
			assert(alpha_px(fb, CLEAR_X(20), 172, 610, 298) ==
			       0u); /* left of the power panel (x 612..712) */
		}
	}
	uint64_t dt = now_ns() - t0;

	/* 8. The per-frame cap (the HE runs with one): a screen change paints
	 * the first dirty tile at least, never more than the cap otherwise,
	 * and a static view converges to the scratch paint within a few frames. */
	{
		tr_hud_t hc;
		uint32_t frames = 0, p1;

		tr_hud_init(&hc);
		hc.budget = 100000u;
		tr_hud_view_set(&v, &s, TR_BANNER_ATTRACT, true, TR_HUD_INVITE_NONE); /* no blink: static */
		do {
			p1 = tr_hud_update(&hc, fb, &v, &dirty);
			/* over the cap only alone: one tile, or the card's middle
			 * as one picture (the page turn) */
			assert(p1 <= hc.budget || (dirty & (dirty - 1u)) == 0u || (dirty & ~MIDDLE) == 0u);
			frames++;
		} while (p1 != 0u && frames < 10u);
		assert(frames >= 3u &&
		       frames <= 5u); /* 253,440 px at <= 100,000 a frame, then one quiet frame */
		tr_hud_paint_all(ref, &v, tr_hz_to40(hc.frame - 1u), hc.popup_start, hc.zone_start);
		assert(memcmp(fb, ref, sizeof(fb)) == 0);
	}

#ifndef TR_PARTNER_LOGO_HEADER
	/* 9. The partner logo's room (hud.c PARTNER_*, measured WITHOUT the logo built in): on every
	 * screen the HUD paints nothing opaque (alpha 15: text, bars) in the logo plate's rows
	 * (y 170 .. 259) left of x 242 on the screens that get the full plate (play, attract), nor left of
	 * x 150 on the ones that get the compact plate (crash, banner, initials, the high-score page: text
	 * nearer the edge). Card backing (alpha < 15) may sit under the plate. A screen that moves its text
	 * left breaks this, and the logo build with it. */
	{
		tr_hiscore_t  hs;
		tr_initials_t ini;
		tr_score_t    ps;
		int           min_full = 999, min_compact = 999;

		tr_hs_init(&hs);
		for (int i = 0; i < 5; i++) {
			(void)tr_hs_insert(&hs, 9999999u - (uint32_t)i * 1000u, "WWW");
		}
		memset(&ini, 0, sizeof(ini));
		strcpy(ini.name, "WWW");
		tr_score_init(&ps);
		ps.score      = 9999999u;
		ps.metres     = 999999u;
		ps.best       = 9999999u;
		ps.combo      = 9u;
		ps.popup_pts  = 500u;
		ps.popup_mult = 5u;
		ps.new_best   = 1u;
		for (int mode = 0; mode < 7; mode++) {
			for (int inv = 0; inv < 3; inv++) {
				for (int ch = 0; ch < 4; ch++) {
					for (int page = 0; page < 2; page++) {
						static const uint8_t ban[7] = { TR_BANNER_NONE,      TR_BANNER_ATTRACT,
							                            TR_BANNER_GAME_OVER, TR_BANNER_STEP_BACK,
							                            TR_BANNER_STAND,     TR_BANNER_CHECK_CAMERA,
							                            TR_BANNER_NONE };
						tr_hud_view_t        pv;
						uint32_t             fr = page ? TR_HUD_PAGE_FRAMES : 10u;
						bool                 compact;

						tr_hud_view_set(&pv, &ps, ban[mode], mode == 1, (uint8_t)inv);
						tr_hud_view_booth(&pv, &hs, NULL);
						if (mode == 6) { /* the initials entry rides on a play view */
							tr_hud_view_set(&pv, &ps, TR_BANNER_NONE, false, TR_HUD_INVITE_NONE);
							pv.mode = TR_HUD_INITIALS;
							tr_hud_view_booth(&pv, &hs, &ini);
						}
						pv.character = (uint8_t)ch;
						tr_hud_view_zone(&pv, 3u, 1u);
						for (int l = 0; l < TR_PERF_LINES; l++) {
							memset(pv.perf[l], 'W', 30);
						}
						tr_hud_paint_all(fb, &pv, fr, fr - 5u, fr - 20u);
						compact = pv.mode == TR_HUD_CRASH || pv.mode == TR_HUD_BANNER ||
						          pv.mode == TR_HUD_INITIALS || (pv.mode == TR_HUD_ATTRACT && page);
						for (int y = 170; y < 260; y++) {
							for (int x = 118; x < TR_HUD_W; x++) {
								if ((fb[y * TR_HUD_W + x] >> 12) == 15u) {
									int *m = compact ? &min_compact : &min_full;

									*m = x < *m ? x : *m;
									break;
								}
							}
						}
					}
				}
			}
		}
		/* hud.c: full plate ends x 238 (+ 4 clear), compact x 146 (+ 4) */
		assert(min_full >= 242 && min_compact >= 150);
		printf("hud: partner room: opaque content from x %d (play, attract), x %d (the rest)\n",
		       min_full,
		       min_compact);
	}
#endif
#ifdef TR_PARTNER_LOGO_HEADER
	/* 10. The optional partner logo (TR_PARTNER_LOGO_HEADER; runner.sh builds this test against a
	 * SYNTHETIC header): its card-style plate down the left edge, in two sizes, in bounds, in the
	 * card's left tiles, clear of the name / invitation row, the same pixels whenever the same size
	 * shows, free per frame. Section 9 (default build) proves the text stays clear of both sizes. */
	{
		static uint16_t      refp[2][TR_HUD_W * TR_HUD_H];
		int                  px_[2], py_[2], pw_[2], ph_[2], screens = 0;
		tr_hud_t             hl;
		tr_hud_view_t        vl;
		tr_score_t           ls;
		tr_hiscore_t         hs;
		tr_initials_t        ini;
		uint32_t             d2;
		static const uint8_t ban[5] = { TR_BANNER_NONE,
			                            TR_BANNER_ATTRACT,
			                            TR_BANNER_GAME_OVER,
			                            TR_BANNER_STEP_BACK,
			                            TR_BANNER_NONE };

		for (int c = 0; c < 2; c++) {
			assert(tr_hud_partner_logo_rect(c != 0, &px_[c], &py_[c], &pw_[c], &ph_[c]));
			assert(px_[c] >= 0 && py_[c] >= 0 && px_[c] + pw_[c] <= TR_HUD_W &&
			       py_[c] + ph_[c] <= TR_HUD_H);
			/* mirrors the power tile (hud.c PWR_Y 170) down the left edge, level with the BEST /
			 * logo cards (SCORE_X 16), under the tagline strip (to 168) and above the name /
			 * invitation row (INV_Y 300), inside the card's left middle tiles (hud.c T_MIDL 0..220
			 * and T_POP 220..500, rows 140..300): only a repaint of those ever touches it */
			assert(px_[c] == 16 && py_[c] == 170 && py_[c] >= 168 && py_[c] + ph_[c] <= 300 &&
			       px_[c] + pw_[c] <= 500);
		}
		assert(pw_[0] == TR_PARTNER_LOGO_W + 2 * TR_HUD_PARTNER_PAD &&
		       ph_[0] == TR_PARTNER_LOGO_H + 2 * TR_HUD_PARTNER_PAD);
		assert(pw_[1] == TR_PARTNER_LOGO_S_W + 2 * TR_HUD_PARTNER_PAD &&
		       ph_[1] == TR_PARTNER_LOGO_S_H + 2 * TR_HUD_PARTNER_PAD);
		assert(TR_PARTNER_LOGO_W <= 210 &&
		       TR_PARTNER_LOGO_H <= 77); /* the largest logos hud.c takes */
		assert(TR_PARTNER_LOGO_S_W <= 118 && TR_PARTNER_LOGO_S_H <= 43);
		assert(px_[0] + pw_[0] <= 238 && px_[1] + pw_[1] <= 146); /* section 9's clearances */
		assert(px_[0] + pw_[0] > 220); /* the full plate spans T_MIDL + T_POP */
		assert((220 + 280) * (300 - 140) <=
		       110000); /* hud_l2.c HUD_PX_BUDGET: both tiles together fit a frame */

		tr_hs_init(&hs);
		(void)tr_hs_insert(&hs, 9999999u, "ZZZ");
		(void)tr_hs_insert(&hs, 1234u, "AAA");
		memset(&ini, 0, sizeof(ini));
		strcpy(ini.name, "WWW");
		memset(&vl, 0, sizeof(vl));
		tr_score_init(&ls);
		/* the references: the full plate on a play screen, the compact one on a crash screen */
		tr_hud_view_set(&vl, &ls, TR_BANNER_NONE, false, TR_HUD_INVITE_NONE);
		assert(!tr_hud_partner_logo_compact(&vl, 10u));
		tr_hud_paint_all(refp[0], &vl, 10u, 0u - 100u, 0u - 200u);
		tr_hud_view_set(&vl, &ls, TR_BANNER_GAME_OVER, false, TR_HUD_INVITE_NONE);
		assert(tr_hud_partner_logo_compact(&vl, 10u));
		tr_hud_paint_all(refp[1], &vl, 10u, 0u - 100u, 0u - 200u);
		for (int c = 0; c < 2; c++) {
			int lw = c ? TR_PARTNER_LOGO_S_W : TR_PARTNER_LOGO_W,
			    lh = c ? TR_PARTNER_LOGO_S_H : TR_PARTNER_LOGO_H;

			/* the HUD's card pixel in the padding, the logo's own pixels where it is opaque, the
			 * card showing through where it is clear */
			assert(refp[c][(py_[c] + 2) * TR_HUD_W + px_[c] + pw_[c] / 2] ==
			       (uint16_t)(10u << 12 | 0x013u));
			for (int y = 0; y < lh; y++) {
				for (int x = 0; x < lw; x++) {
					uint16_t g =
					    c ? TR_PARTNER_LOGO_S_PX(y * lw + x) : TR_PARTNER_LOGO_PX(y * lw + x);
					uint16_t o = refp[c][(py_[c] + TR_HUD_PARTNER_PAD + y) * TR_HUD_W + px_[c] +
					                     TR_HUD_PARTNER_PAD + x];

					assert(g >> 12 != 15u || o == g);
					assert(g >> 12 != 0u || o == (uint16_t)(10u << 12 | 0x013u));
				}
			}
		}
		ls.score      = 9999999u;
		ls.metres     = 999999u;
		ls.best       = 9999999u;
		ls.combo      = 9u;
		ls.popup_pts  = 500u;
		ls.popup_mult = 5u;
		ls.new_best   = 1u;
		/* Every screen x invitation x character x popup/zone x table page, widest numbers and
		 * perf lines: the plate is the same pixels whenever the same size shows (it is drawn
		 * last, so a card never cuts it), and the size is the compact one on exactly the crash,
		 * banner, initials and high-score screens. */
		for (int mode = 0; mode < 5; mode++) {
			for (int inv = 0; inv < 3; inv++) {
				for (int ch = 0; ch < 4; ch++) {
					for (int pop = 0; pop < 2; pop++) {
						for (int page = 0; page < 2; page++) {
							uint32_t fr = page ? TR_HUD_PAGE_FRAMES : 10u;
							bool     cp;

							tr_hud_view_set(&vl, &ls, ban[mode], mode == 1, (uint8_t)inv);
							if (mode == 4) {
								vl.mode = TR_HUD_INITIALS;
							}
							tr_hud_view_booth(&vl, &hs, mode == 4 ? &ini : NULL);
							vl.character = (uint8_t)ch;
							tr_hud_view_zone(&vl, (uint8_t)(pop ? 3 : 0), (uint32_t)pop);
							for (int l = 0; l < TR_PERF_LINES; l++) {
								memset(vl.perf[l], 'W', 30);
							}
							cp = tr_hud_partner_logo_compact(&vl, fr);
							assert(cp == (mode == 2 || mode == 3 || mode == 4 ||
							              (mode == 1 && page && vl.hs.n != 0u)));
							tr_hud_paint_all(
							    fb, &vl, fr, pop ? fr - 5u : 0u - 100u, pop ? fr - 20u : 0u - 200u);
							for (int y = 0; y < ph_[cp]; y++) {
								assert(memcmp(&fb[(py_[cp] + y) * TR_HUD_W + px_[cp]],
								              &refp[cp][(py_[cp] + y) * TR_HUD_W + px_[cp]],
								              (size_t)pw_[cp] * 2u) == 0);
							}
							screens++;
						}
					}
				}
			}
		}
		/* Always on, incrementally: attract -> play -> crash, the plate (and its size) follow the
		 * screen and the update still equals a scratch paint. */
		tr_hud_init(&hl);
		for (uint32_t f = 0; f < 400u; f++) {
			bool cp;

			ls.popup_pts = 0u;
			tr_hud_view_set(&vl,
			                &ls,
			                f < 120u   ? TR_BANNER_ATTRACT
			                : f < 260u ? TR_BANNER_NONE
			                           : TR_BANNER_GAME_OVER,
			                f < 120u,
			                TR_HUD_INVITE_STEP_IN);
			tr_hud_view_booth(&vl, &hs, NULL);
			(void)tr_hud_update(&hl, fb, &vl, &d2);
			same_as_scratch(&hl, &vl);
			cp = tr_hud_partner_logo_compact(&vl, tr_hz_to40(hl.frame - 1u));
			for (int y = 0; y < ph_[cp]; y++) {
				assert(memcmp(&fb[(py_[cp] + y) * TR_HUD_W + px_[cp]],
				              &refp[cp][(py_[cp] + y) * TR_HUD_W + px_[cp]],
				              (size_t)pw_[cp] * 2u) == 0);
			}
		}
		/* A play popup repaints T_POP every frame (x 220 .. 500: the full plate's right edge is in
		 * it): the plate stays whole, and that tile alone repaints. */
		tr_hud_init(&hl);
		tr_score_init(&ls);
		for (uint32_t f = 0; f < 40u; f++) {
			memset(&vl, 0, sizeof(vl));
			ls.popup_seq = f < 4u ? 0u : 1u;
			ls.popup_pts = 50u;
			tr_hud_view_set(&vl, &ls, TR_BANNER_NONE, false, TR_HUD_INVITE_NONE);
			(void)tr_hud_update(&hl, fb, &vl, &d2);
			same_as_scratch(&hl, &vl);
			if (f > 8u && f < 4u + TR_HUD_POPUP_FRAMES - 1u) {
				assert(d2 == (1u << 4)); /* hud.c T_POP only */
			}
			for (int y = 0; y < ph_[0]; y++) {
				assert(memcmp(&fb[(py_[0] + y) * TR_HUD_W + px_[0]],
				              &refp[0][(py_[0] + y) * TR_HUD_W + px_[0]],
				              (size_t)pw_[0] * 2u) == 0);
			}
		}
		/* No per-frame cost: a static view repaints nothing, and a change elsewhere (the perf
		 * panel, hud.c T_PERF) repaints that tile alone -- the plate's tiles are not in its key. */
		tr_hud_init(&hl);
		memset(&vl, 0, sizeof(vl));
		tr_score_init(&ls);
		tr_hud_view_set(&vl, &ls, TR_BANNER_NONE, false, TR_HUD_INVITE_NONE);
		(void)tr_hud_update(&hl, fb, &vl, NULL);
		assert(tr_hud_update(&hl, fb, &vl, NULL) == 0u);
		snprintf(vl.perf[0], TR_PERF_COLS, "FPS 40.0");
		assert(tr_hud_update(&hl, fb, &vl, &d2) > 0u && d2 == (1u << 2));
		printf("hud: partner plates %d x %d / %d x %d at (%d, %d), checked on %d screens\n",
		       pw_[0],
		       ph_[0],
		       pw_[1],
		       ph_[1],
		       px_[0],
		       py_[0],
		       screens);
	}
#endif

	printf("hud: %u px repainted over %u frames, host %.2f ns/px (%.1f ms total)\n",
	       (unsigned)n,
	       TR_HUD_POPUP_FRAMES + 403u,
	       (double)dt / (double)n,
	       (double)dt / 1e6);
	/* Worst single frame: a screen change repaints the whole HUD. */
	tr_hud_init(&h);
	t0 = now_ns();
	px = tr_hud_update(&h, fb, &v, &dirty);
	dt = now_ns() - t0;
	printf("hud: full repaint %u px, host %.3f ms\n", (unsigned)px, (double)dt / 1e6);
	return 0;
}
