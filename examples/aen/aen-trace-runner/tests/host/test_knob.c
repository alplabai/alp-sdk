/* tests/host/test_knob.c -- src/ipc/tr_knob.c: the encoder switch's short / long press (with
 * debounce), the BRIGHTNESS mode and its 5 s revert, the 10..100 % clamps, a long press that
 * mutes and restores through tr_vol_he_step, a short press while muted, the bench's request word
 * and the HUD popup's text. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/hud/hud.h"
#include "../../src/ipc/tr_knob.h"

static volatile tr_bl_t  rec;
static volatile tr_vol_t vrec;
static tr_knob_t         k;
static tr_vol_he_t       he;
static uint32_t          t; /* the clock, ms */

static void fresh(uint32_t bl)
{
	memset((void *)&rec, 0xA5, sizeof(rec)); /* cold SRAM0 */
	memset((void *)&vrec, 0xA5, sizeof(vrec));
	tr_knob_boot(&k, &rec, bl, true);
	tr_vol_he_boot(&he, &vrec);
	t = 100000u;
}

/* One 25 ms frame: the knob, then what the HE does with its output. */
static tr_knob_out_t frame(int32_t detents, bool sw)
{
	tr_knob_out_t o = tr_knob_step(&k, &rec, t, detents, sw);

	(void)tr_vol_he_step(&he, &vrec, o.vol_detents, o.mute);
	t += 25u;
	return o;
}

static void hold(bool sw, uint32_t ms)
{
	for (uint32_t e = t + ms; (int32_t)(e - t) > 0;) {
		(void)frame(0, sw);
	}
}

static void press_short(void)
{
	hold(true, 200u);
	hold(false, 100u);
}

static void press_long(void)
{
	hold(true, TR_KNOB_LONG_MS + 200u);
	hold(false, 100u);
}

static void test_boot(void)
{
	fresh(30u);
	assert(k.mode == TR_KNOB_VOLUME && k.bl_pct == 30u && rec.bl == tr_bl_word(30u));
	assert(rec.rejects == 0u && rec.seq == 0u);
	tr_knob_boot(&k, &rec, 3u, true); /* below the floor: raised, never dark */
	assert(k.bl_pct == TR_BL_MIN);
	tr_knob_boot(&k, &rec, 250u, true);
	assert(k.bl_pct == TR_BL_MAX);
	assert(TR_MEM_BL == 0x0237FDC0u && TR_BL_TAG == 0x424C0000u);
}

static void test_press_classes(void)
{
	fresh(30u);
	press_short(); /* released well before TR_KNOB_LONG_MS */
	assert(k.mode == TR_KNOB_BRIGHTNESS && he.pct == TR_VOL_DEFAULT && !he.muted);
	press_short();
	assert(k.mode == TR_KNOB_VOLUME);

	/* just under the long threshold is still short, at it is long */
	fresh(30u);
	hold(true, TR_KNOB_LONG_MS - 100u);
	hold(false, 100u);
	assert(k.mode == TR_KNOB_BRIGHTNESS && !he.muted && he.pct == 30u);
	fresh(30u);
	hold(true, TR_KNOB_LONG_MS + 100u);
	assert(he.pct == 0u && he.muted); /* fires while held */
	hold(false, 100u);                /* the release after a long press toggles nothing */
	assert(k.mode == TR_KNOB_VOLUME && he.pct == 0u);
}

static void test_debounce(void)
{
	fresh(30u);
	/* bounces shorter than TR_KNOB_DEBOUNCE_MS never become a press */
	for (int i = 0; i < 10; i++) {
		(void)tr_knob_step(&k, &rec, t, 0, true);
		t += 10u;
		(void)tr_knob_step(&k, &rec, t, 0, false);
		t += 10u;
	}
	hold(false, 200u);
	assert(!k.down && k.mode == TR_KNOB_VOLUME && k.seq == 0u);
	/* a press whose contact chatters at the start still counts once, as one short press */
	(void)tr_knob_step(&k, &rec, t, 0, true);
	t += 10u;
	(void)tr_knob_step(&k, &rec, t, 0, false);
	t += 10u;
	hold(true, 200u);
	hold(false, 100u);
	assert(k.mode == TR_KNOB_BRIGHTNESS);
	/* a chatter in the middle of a press does not split it */
	fresh(30u);
	hold(true, 100u);
	(void)tr_knob_step(&k, &rec, t, 0, false);
	t += 10u;
	hold(true, 200u);
	hold(false, 100u);
	assert(k.mode == TR_KNOB_BRIGHTNESS && k.seq == 1u);
}

static void test_brightness(void)
{
	fresh(30u);
	assert(frame(3, false).vol_detents == 3 && !frame(0, false).bl_changed); /* VOLUME: to volume */
	press_short();
	tr_knob_out_t o = frame(1, false);
	assert(o.bl_changed && o.vol_detents == 0 && k.bl_pct == 35u && rec.bl == tr_bl_word(35u));
	assert(rec.seq == 1u);
	assert(frame(-2, false).bl_changed && k.bl_pct == 25u);
	/* clamps: 10 is the floor, 100 the ceiling, a wild burst included */
	assert(frame(-1000000, false).bl_changed && k.bl_pct == TR_BL_MIN);
	assert(!frame(-1, false).bl_changed && k.bl_pct == TR_BL_MIN && rec.bl == tr_bl_word(10u));
	assert(frame(2147483647, false).bl_changed && k.bl_pct == TR_BL_MAX);
	assert(!frame(1, false).bl_changed && k.bl_pct == TR_BL_MAX);
	assert(frame((-2147483647 - 1), false).bl_changed && k.bl_pct == TR_BL_MIN);
	assert(frame(1, false).bl_changed && k.bl_pct == 15u);
	/* the volume did not move */
	assert(he.pct == 30u + 15u);
	/* the bench cannot move the level by writing bl */
	rec.bl = 0u;
	(void)frame(0, false);
	assert(rec.bl == tr_bl_word(15u));
}

static void test_idle_revert(void)
{
	fresh(30u);
	press_short();
	assert(k.mode == TR_KNOB_BRIGHTNESS);
	hold(false, TR_KNOB_IDLE_MS - 400u);
	assert(k.mode == TR_KNOB_BRIGHTNESS); /* not yet */
	(void)frame(1, false);                /* a turn restarts the count */
	hold(false, TR_KNOB_IDLE_MS - 400u);
	assert(k.mode == TR_KNOB_BRIGHTNESS && k.bl_pct == 35u);
	hold(false, 600u);
	assert(k.mode == TR_KNOB_VOLUME && k.bl_pct == 35u); /* reverts, keeps the level */
	assert(frame(1, false).vol_detents == 1);
	/* a short press is activity too: the count restarts at its release */
	press_short();
	hold(false, TR_KNOB_IDLE_MS - 400u);
	assert(k.mode == TR_KNOB_BRIGHTNESS);
	hold(false, 600u);
	assert(k.mode == TR_KNOB_VOLUME);
}

static void test_mute(void)
{
	fresh(30u);
	(void)frame(2, false);
	assert(he.pct == 40u);
	press_long();
	assert(he.pct == 0u && vrec.vol == tr_vol_word(0u));
	(void)frame(1, false); /* a turn while muted unmutes and steps from the saved level */
	assert(he.pct == 45u);
	press_long();
	assert(he.pct == 0u);
	press_long(); /* a second long press restores the level it muted */
	assert(he.pct == 45u);
	/* a long press from BRIGHTNESS mutes and the popup is about the volume again */
	press_short();
	assert(k.mode == TR_KNOB_BRIGHTNESS);
	press_long();
	assert(he.pct == 0u && k.mode == TR_KNOB_VOLUME);
}

static void test_short_while_muted(void)
{
	fresh(30u);
	press_long();
	assert(he.pct == 0u && he.muted);
	press_short(); /* still muted: only the mode moves */
	assert(he.pct == 0u && he.muted && k.mode == TR_KNOB_BRIGHTNESS);
	(void)frame(1, false); /* brightness mode: the turn is brightness, not an unmute */
	assert(he.pct == 0u && k.bl_pct == 35u);
	press_short();
	assert(he.pct == 0u && k.mode == TR_KNOB_VOLUME);
	press_long(); /* only a long press unmutes */
	assert(he.pct == 30u && !he.muted);
}

static void test_request(void)
{
	fresh(30u);
	assert(!frame(0, false).bl_changed); /* the 0xA5A5A5A5 req left over: not a request */
	rec.req         = tr_bl_word(60u);
	tr_knob_out_t o = frame(0, false);
	assert(o.bl_changed && k.bl_pct == 60u && rec.bl == tr_bl_word(60u) &&
	       k.mode == TR_KNOB_BRIGHTNESS);
	assert(!frame(0, false).bl_changed); /* same word again: nothing */
	rec.req = TR_BL_TAG | 5u;            /* under the floor */
	assert(!frame(0, false).bl_changed && rec.rejects == 1u && k.bl_pct == 60u);
	rec.req = TR_BL_TAG | 0u; /* a bare zero is not "dark" */
	assert(!frame(0, false).bl_changed && rec.rejects == 2u);
	rec.req = TR_BL_TAG | 101u;
	assert(!frame(0, false).bl_changed && rec.rejects == 3u);
	rec.req = 0x564F0028u; /* the volume tag */
	assert(!frame(0, false).bl_changed && rec.rejects == 4u && k.bl_pct == 60u);
	rec.req = 0u; /* re-arm */
	(void)frame(0, false);
	rec.req = tr_bl_word(60u);
	assert(!frame(0, false).bl_changed && rec.rejects == 4u); /* same level: no change, no reject */
	assert(tr_bl_valid(tr_bl_word(10u), &(uint32_t){ 0 }) &&
	       tr_bl_valid(tr_bl_word(100u), &(uint32_t){ 0 }));
}

static void test_no_backlight(void)
{
	fresh(30u);
	tr_knob_boot(&k, &rec, 30u, false); /* the RK055 shield: no PWM backlight */
	press_short();
	assert(k.mode == TR_KNOB_VOLUME && k.seq == 0u);
	rec.req = tr_bl_word(60u);
	assert(!frame(0, false).bl_changed && k.bl_pct == 30u);
	press_long(); /* the mute still works */
	assert(he.pct == 0u);
}

static void test_popup_text(void)
{
	char b[24];

	tr_hud_vol_text(b, sizeof(b), TR_HUD_KNOB_VOLUME, 30u);
	assert(strcmp(b, "VOLUME 30%") == 0);
	tr_hud_vol_text(b, sizeof(b), TR_HUD_KNOB_VOLUME, 0u);
	assert(strcmp(b, "MUTE") == 0);
	tr_hud_vol_text(b, sizeof(b), TR_HUD_KNOB_BRIGHTNESS, 60u);
	assert(strcmp(b, "BRIGHTNESS 60%") == 0);
	tr_hud_vol_text(b, sizeof(b), TR_HUD_KNOB_BRIGHTNESS, 100u);
	assert(strcmp(b, "BRIGHTNESS 100%") == 0 && strlen(b) < sizeof(b));
	tr_hud_vol_text(b, sizeof(b), TR_HUD_KNOB_BRIGHTNESS, 10u);
	assert(strcmp(b, "BRIGHTNESS 10%") == 0);
	/* the longest popup fits the HUD row */
	assert(tr_hud_text_w(TR_HUD_FONT_MED, "BRIGHTNESS 100%") + 24 < TR_HUD_W);
}

int main(void)
{
	test_boot();
	test_press_classes();
	test_debounce();
	test_brightness();
	test_idle_revert();
	test_mute();
	test_short_while_muted();
	test_request();
	test_no_backlight();
	test_popup_text();
	printf("knob: ok\n");
	return 0;
}
