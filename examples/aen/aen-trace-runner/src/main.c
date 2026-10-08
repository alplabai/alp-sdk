/* src/main.c */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "game/attract.h"
#include "game/hiscore.h"
#include "game/mode.h"
#include "game/ramp.h"
#include "game/react.h"
#include "game/score.h"
#include "game/state.h"
#include "game/tilt.h"
#include "game/zone.h"
#include "ipc/tr_aring.h" /* TR_AEV_* -- sound events (A32 build pushes them) */
#include "ipc/tr_mbox.h"  /* TR_BANNER_* -- the banner ids both render modes share */
#include "platform/display.h"
#include "platform/imu.h"
#include "platform/bus2_he.h"
#include "platform/rail5v_power.h"
#include "vision/camera_watchdog.h"
#include "vision/pose.h" /* the presence rule, both input paths */
#include "vision/track.h"
#if TR_INPUT_NPU
/* The HP owns the camera + NPU now (docs/superpowers/specs/
 * 2026-09-24-npu-body-control-design.md sec 5): no local camera or classical
 * detector, just the pose slot it publishes into. */
#include <zephyr/sys/barrier.h>

#include "ipc/tr_memmap.h" /* TR_MEM_PSLOT */
#include "ipc/tr_cam_view.h"
#include "ipc/tr_pslot.h"
#include "vision/cam_rot.h"

/* The HE's side of the I2C1 handover (i2c_handover_he.overlay) names the same
 * flag word hp_vision waits on (tr_memmap.h). */
BUILD_ASSERT(DT_PROP(DT_NODELABEL(i2c1_handover), flag_address) == TR_MEM_I2C1_HANDOVER,
             "i2c1_handover flag-address != tr_memmap.h TR_MEM_I2C1_HANDOVER");
#else
#include "platform/camera.h"
#include "vision/detect.h"
#endif
#if TR_RENDER_A32
#include <zephyr/sys/barrier.h>

#include "game/sfx.h"
#include "hud/hud.h"
#include "platform/a32.h"
#include "platform/hud_l2.h"
#else
#include "render/render.h"
#endif

/*
 * ~30 Hz logic against the panel's measured 40.0 Hz refresh -- TR_RENDER=M55
 * only. TR_RENDER=A32 does not sleep it out: the flip's vblank wait paces the
 * loop, so the game runs at the flip rate (40.0 Hz when the A32 keeps up;
 * 30.0 Hz on a 30 Hz panel, game/panel_hz.h scaling the frame counts) and
 * every tick-counted constant below and in the game (TR_AIR_TICKS,
 * TR_SCROLL_PX, TR_CALIB_TIMEOUT_TICKS, ...) runs ~1.2x faster in wall time.
 * Retuning them is plan task T9, not done here; TICK_MS still scales the
 * printed fallback time.
 */
#define TICK_MS      33
#define TR_CAM_DECIM 10 /* 640x400 frame -> 64x40 = 2560 cells; see detect.h's header comment. */

#define TR_DISPLAY_OPEN_RETRIES \
	5                           /* See tr_display_open()'s retry loop (whole-branch review F13). */
#define TR_DISPLAY_RETRY_MS 200 /* Pause between attempts -- a guess at a power-up settling time. */

/*
 * Bound on the calibration retry loop below: 450 ticks * TICK_MS(33 ms) =
 * 14,850 ms -- a FLOOR of ~15 s, not the actual figure: each iteration also
 * costs a banner blit and a capture_box() (a camera dequeue plus a detect
 * pass), so real wall-clock time is longer by that much per tick and depends
 * on blit/capture cost. The error direction is safe (longer, never shorter,
 * so it cannot fire early on a slow player). A player who is merely slow to
 * get into position clears calibration in a couple of seconds by moving; a
 * camera that is dead or permanently occluded never will, no matter how long
 * the loop waits, so past this many ticks without success the game shows
 * "CHECK THE CAMERA" and falls back to TR_FALLBACK_MODE (mode.h -- attract by
 * default, tilt on a bench build) instead of sitting on a lit bar forever --
 * see task-11-brief.md Step 2b.
 */
#define TR_CALIB_TIMEOUT_TICKS 450
/* STEP BACK hint must hold this many ticks (~170 ms at 30 Hz) before it shows. */
#define TR_STEP_BACK_HOLD_TICKS 5u
#define TR_FALLBACK_BANNER_MS \
	2000 /* How long CHECK THE CAMERA stays up before play starts in the fallback mode. */

/*
 * Whole-branch review F6: the RAM console (the only diagnostic channel this
 * board has -- CONFIG_UART_CONSOLE=n) is 8,191 bytes with no wrap marker.
 * A per-run line every run, plus an fps line every 3 s, wrapped it in about
 * 10 minutes and erased the one-shot boot/mode/calibration lines that
 * actually explain a bad demo. The periodic fps line is gone outright; the
 * run-over line prints only every Nth run.
 */
#define TR_RUN_OVER_PRINT_EVERY 10

#if !TR_INPUT_NPU
/*
 * 8,204 bytes (TR_DETECT_GRID_MAX * sizeof(int16_t) plus a few scalars)
 * against an 8,192-byte CONFIG_MAIN_STACK_SIZE -- MUST be static, never a
 * main() local, or it overflows the stack before any other local is even
 * allocated. See detect.h's comment on tr_detect_t; test_detect.c's
 * file-scope s_detect is the pattern this copies.
 */
static tr_detect_t g_detect;
#else
/* TR_INPUT_NPU: the HP -> HE pose slot (tr_pslot.h), read in capture_box()
 * below. g_pslot_seq is the last seq this core accepted; g_pslot_stale is
 * how many consecutive HE ticks have passed with no NEW seq -- design sec 3:
 * "a stale pose counts as invalid: slot seq unchanged for more than 5 HE
 * ticks", independent of and much shorter than the general
 * TR_CALIB_TIMEOUT_TICKS give-up-on-vision-mode-entirely timeout below. */
#define TR_NPU_STALE_TICKS 5
static volatile tr_pslot_t *const g_pslot = (volatile tr_pslot_t *)TR_MEM_PSLOT;
static uint32_t                   g_pslot_seq;
static uint32_t g_pslot_stale = TR_NPU_STALE_TICKS + 1u; /* no pose read yet: start stale */

static void pslot_barrier(void)
{
	barrier_dsync_fence_full();
}

/* Whether the upright view the poses are in is mirrored like a selfie: what
 * decides which arm is the player's left (vision/pose.c). The truth is the HP's
 * own report in its camera descriptor (tr_cam_view_t.mirror, the sensor flip bit
 * read back after every camera open) -- not this build's TR_CAM_MIRROR, which
 * only says what the two images were BUILT to agree on. Until a valid
 * descriptor has been read the build's value stands; the last valid read is
 * kept across a torn one. A disagreement is printed once, loudly. */
static volatile const tr_cam_view_t *const g_cam_view =
    (volatile const tr_cam_view_t *)TR_MEM_CAM_VIEW;
static bool g_mirrored = TR_CAM_MIRROR != 0;
static bool g_mirror_warned;

static bool hp_mirrored(void)
{
	tr_cam_view_t cv;

	if (tr_cam_view_read(g_cam_view, &cv, pslot_barrier)) {
		g_mirrored = cv.mirror != 0u;
		if (!g_mirror_warned &&
		    (g_mirrored != (TR_CAM_MIRROR != 0) || (cv.rotate == 0u) != (TR_CAM_ROTATE == 0))) {
			g_mirror_warned = true;
			printk(
			    "!!!!! HE built for TR_CAM_MIRROR=%d TR_CAM_ROTATE=%d but the HP reports mirror=%u "
			    "rotate=%u -- using the HP's mirror for the arm controls; rebuild the HE to "
			    "match\n",
			    TR_CAM_MIRROR,
			    TR_CAM_ROTATE,
			    (unsigned)cv.mirror,
			    (unsigned)cv.rotate);
		}
	}
	return g_mirrored;
}

/* Last successfully accepted publish -- capture_box() below must NEVER read
 * its own stack-local tr_pslot_read() `out` on a failed read (about every
 * other tick: the HE ticks at TICK_MS while the HP publishes on its own,
 * unsynchronised cadence, so "nothing new this tick" is the common case,
 * not an edge case): that leaves `out` uninitialised and reads it anyway
 * (reviewer finding, fix round 1). Static, zero-initialised: harmless even
 * before the first real publish, because g_pslot_stale below (already
 * TR_NPU_STALE_TICKS + 1 at boot) gates capture_box() to invalid first. */
static tr_pslot_t g_pslot_last;

/* No local camera object on this core to ask (platform/camera.h is not
 * linked into a TR_INPUT_NPU build) -- track.c's duck (and its TR_CAM_FLIP_Y
 * mirror) still needs the frame's height: the HP's fixed OV9281 mode turned
 * UPRIGHT (src/vision/cam_rot.h -- 400x640 with the sensor on its side,
 * 640x400 when it is not), the frame the pose keypoints come in. Kept as same-named
 * functions rather than replacing every call site below, so the
 * mode-selection/calibration code reads identically on both paths. */
static inline int16_t tr_camera_width(void)
{
	return TR_CAM_UP_W(TR_CAM_ROTATE);
}

static inline int16_t tr_camera_height(void)
{
	return TR_CAM_UP_H(TR_CAM_ROTATE);
}
#endif

/* Sub-tick phase, Q0.16: how far the world is past the last game step
 * toward the next (state.h TR_PLAY_SPEED_Q16 in play, attract.h
 * TR_ATTRACT_SPEED_Q16 in attract, both ramped up as a run goes on:
 * game/ramp.h tr_ramp_frame_q16()); the A32 interpolates by it. g_pace_q8: the pace this frame. */
static uint32_t g_phase_q16;
static uint8_t  g_pace_q8 = (uint8_t)(TR_PLAY_SPEED_Q16 >> 8);

/* The points the player sees (game/score.h): the run's, and the session best. */
static tr_score_t g_score;

/* The booth's high-score table (game/hiscore.h): RAM only, since boot. A
 * player run that makes it gets the initials entry (enter_high_score()),
 * shown on the HUD through g_ini while it runs. */
static tr_hiscore_t         g_hs;
static const tr_initials_t *g_ini;

/* The world zone (P15, game/zone.h): stepped with the game, in play and in
 * attract alike (a demo run's crash does not restart it: a passer-by sees
 * every zone), back to the board when a player's run starts or ends. */
static tr_zone_t g_zone;

/* P16. The runner's current reaction (game/react.h), fed by every game step
 * of a run and aged every presented frame; the attract lobby
 * (game/attract.h: the runner idles on every way into attract and after
 * each demo run -- A32 render only: the M55 sprite renderer has no idle
 * animation, it would just freeze the runner). */
static tr_react_t g_react;
static tr_lobby_t g_lobby;

/* The character lives in the tilt state (game/tilt.h: attract cycles it
 * per demo run, a tilt pick sets it); defined with the bench globals below. */
extern tr_tilt_t tr_tilt;

/* A new run's first state: points, reaction. Not the world zone
 * (g_zone): a demo run's end keeps it (a passer-by sees every zone), so
 * each caller resets it where a player's run starts. */
static void run_start(tr_game_t *g)
{
	tr_game_init(g, (uint32_t)k_cycle_get_32());
	tr_score_run_start(&g_score);
	tr_hs_arm(&g_score, &g_hs); /* the celebration popup when the run passes the table's best */
	tr_react_init(&g_react);
}

/*
 * Presentation seam -- the only place the two TR_RENDER modes differ.
 *
 * M55: render.c draws into the back buffer and tr_display_flip() shows it.
 * A32: nothing is drawn on this core. ui_banner() records which prompt the
 * frame carries, and ui_present() hands the game snapshot plus that banner id
 * to the A32 (platform/a32.h), which renders the whole frame -- so ui_reset()
 * and ui_frame() have nothing to do.
 */
#if TR_RENDER_A32
static uint8_t g_banner = TR_BANNER_NONE;
static uint8_t g_invite = TR_HUD_INVITE_NONE;

static void ui_reset(void)
{
}

/* The attract screen's call to action for this control mode: a camera
 * build invites a step in; a tilt-takeover build invites a tilt; an
 * exhibition build with neither has nothing that could take over. */
static void ui_invite(tr_mode_t mode)
{
	g_invite = mode == TR_MODE_VISION ? TR_HUD_INVITE_STEP_IN
	           : TR_TILT_TAKEOVER     ? TR_HUD_INVITE_TILT
	                                  : TR_HUD_INVITE_NONE;
}

static void ui_banner(uint8_t id)
{
	g_banner = id;
}

static void ui_frame(const tr_game_t *g)
{
	(void)g;
}

static void ui_present(const tr_game_t *g, bool attract_active, bool paused)
{
	tr_frame_in_t in;

	/* Every presenting loop (the crash sequence, the high-score entry, the fallback banner) keeps
	 * the HP's I2C2 lease moving, not only main's frame loop: an HE that stopped ticking would
	 * neither offer the bus nor take it back (platform/bus2_he.h). */
	tr_bus2_he_frame();

	tr_frame_in_from_game(&in, g, g_banner, attract_active, paused);
	tr_frame_in_p16(&in, tr_tilt.character, &g_react, g_lobby.standing, g_lobby.idle_us);
	in.track_h  = (int16_t)tr_display_height();
	in.rotation = tr_display_rotation(); /* the A32 turns the frame by it */
	in.phase    = (uint16_t)g_phase_q16;
	in.pace_q8  = g_pace_q8;
	if (in.phase != 0u) {
		in.flags |= TR_FLAG_PHASE;
	}
	tr_frame_in_set_zone(&in, &g_zone);
	if (tr_hud_l2_up()) {
		in.flags |= TR_FLAG_HUD_L2; /* score + banners are on layer 2: the A32 draws the 3D only */
	}
	tr_a32_present(&in);
	/* Published: the A32 renders this frame now, and the HUD repaint
	 * overlaps it instead of the flip wait (P9). */
	tr_hud_l2_present(
	    &g_score, g_banner, attract_active, g_invite, &g_zone, tr_tilt.character, &g_hs, g_ini);
	g_banner = TR_BANNER_NONE; /* banners are per frame, as in M55 mode's repaints */
	tr_react_frame(&g_react);  /* the reaction clock: real time a frame */
}

/* Hold the presented picture for `ms`: a published frame only reaches the
 * glass when flushed, and the hold is not a missed refresh. */
static void ui_hold(int32_t ms)
{
	tr_a32_flush();
	k_msleep(ms);
	tr_display_pace_reset();
}

static void tick_sleep(int32_t ms)
{
	(void)ms; /* the flip's vblank wait paces the loop */
}

/* The crash sequence after the fatal tick's frame (crash_ticks 0, already
 * presented): fresh frames at the flip rate, the A32 animating knock-back,
 * sparks, shake and flash from each packet's crash_tick, until
 * TR_CRASH_TICKS frames are out. The world is frozen (g is dead), and the
 * stream never stalls, so no pace reset. */
static void ui_crash(tr_game_t *g, bool attract_active)
{
	/* One crash tick a frame whatever the game pace (state.h
	 * tr_game_crash_frame(): ~1.5 s real time, phase 0, the packet at
	 * 1.0x so the sparks fly in real time too). */
	g_pace_q8 = 0u;
	while (tr_game_crash_frame(g, &g_phase_q16)) {
		ui_banner(TR_BANNER_GAME_OVER);
		ui_present(g, attract_active, false);
	}
}

/* Sound (P10): game events into the SRAM0 ring the M55-HP sound firmware
 * (sound/) drains -- src/ipc/tr_aring.h. A32 build only: the M55 build's
 * lcd_fb covers the ring's address. With no HP running the ring just fills
 * and further pushes count as dropped; nothing here waits on the HP. */
static tr_sfx_watch_t g_sfx;

static void sfx_barrier(void)
{
	barrier_dsync_fence_full();
}

/* The initials a high score starts on: the character's (hud.c). */
static const char *ui_default_name(void)
{
	return tr_hud_char_name(tr_tilt.character);
}

/* Something to show the initials entry on: the HUD layer. */
static bool ui_hud_up(void)
{
	return tr_hud_l2_up();
}

static void sfx_init(void)
{
	tr_aring_init((volatile tr_aring_t *)TR_ARING_ADDR, sfx_barrier);
	tr_sfx_watch_init(&g_sfx);
}

static void sfx_push(uint8_t kind, uint8_t param)
{
	(void)tr_aring_push((volatile tr_aring_t *)TR_ARING_ADDR, kind, param, sfx_barrier);
}

static void sfx_frame(const tr_game_t *g, bool attract_on)
{
	tr_aev_t ev[TR_SFX_MAX_EVENTS];
	unsigned n =
	    tr_sfx_watch(&g_sfx, g, g_score.combo, attract_on, (int16_t)tr_display_height(), ev);

	for (unsigned i = 0; i < n; i++) {
		sfx_push(ev[i].kind, ev[i].param);
	}
}
#else
static void ui_reset(void)
{
	tr_render_init(tr_display_width(), tr_display_height());
}

static void ui_invite(tr_mode_t mode)
{
	(void)mode; /* the sprite banner says STEP IN TO PLAY in every mode */
}

static void ui_banner(uint8_t id)
{
	switch (id) {
	case TR_BANNER_STAND:
		tr_render_banner(tr_banner_stand());
		break;
	case TR_BANNER_STEP_BACK:
		tr_render_banner(tr_banner_step_back());
		break;
	case TR_BANNER_ATTRACT:
		tr_render_banner(tr_banner_attract());
		break;
	case TR_BANNER_GAME_OVER:
		tr_render_banner(tr_banner_game_over());
		break;
	case TR_BANNER_CHECK_CAMERA:
		tr_render_banner(tr_banner_check_camera());
		break;
	default:
		break;
	}
}

static void ui_frame(const tr_game_t *g)
{
	tr_render_frame(g);
}

static void ui_present(const tr_game_t *g, bool attract_active, bool paused)
{
	(void)g;
	(void)attract_active;
	(void)paused;
	tr_bus2_he_frame(); /* as the A32 variant above */
	tr_display_flip();
}

static void ui_hold(int32_t ms)
{
	k_msleep(ms);
}

static void tick_sleep(int32_t ms)
{
	k_msleep(ms);
}

/* M55 sprite renderer: no crash animation -- the frozen last frame under
 * the banner, as before. It also has no sub-tick interpolation, so its
 * 0.7x attract (tr_attract_pace) repeats about every third frame: a
 * visible judder, accepted -- the M55 build is not the shipped one. */
static void ui_crash(tr_game_t *g, bool attract_active)
{
	ui_banner(TR_BANNER_GAME_OVER);
	ui_present(g, attract_active, false); /* its own flip: it is drawn after this tick's */
	ui_hold(1500);
}

/* No HUD, no initials entry: every high score is the board's. */
static const char *ui_default_name(void)
{
	return "E8 ";
}

static bool ui_hud_up(void)
{
	return false;
}

static void sfx_init(void)
{
}

static void sfx_push(uint8_t kind, uint8_t param)
{
	(void)kind;
	(void)param;
}

static void sfx_frame(const tr_game_t *g, bool attract_on)
{
	(void)g;
	(void)attract_on;
}
#endif

/* What the A32 draws under a banner before a run exists (title, calibration,
 * fallback): an empty track. Unused by M55 mode's ui_present(). */
static const tr_game_t g_idle;

/* Bench-readable per-tick cost (k_cycle_get_64 cycles, CPU clock), accumulated
 * since boot: tr_cyc_render = game step + every draw before the flip;
 * tr_cyc_work = the whole tick up to and including the flip (excludes the
 * TICK_MS sleep).  Divide by tr_ticks for the mean. */
volatile uint64_t tr_cyc_render;
volatile uint64_t tr_cyc_work;
volatile uint32_t tr_ticks;

/* Bench-readable tilt takeover state (see game/tilt.h): tr_tilt.playing,
 * .engages, .walkaways, .gestures; tr_bench_mode is tr_mode_t (0 VISION,
 * 1 TILT, 2 ATTRACT), tr_bench_lane the game's lane. Live IMU sample:
 * platform/imu.c's tr_imu_x_q8/tr_imu_y_q8. */
tr_tilt_t        tr_tilt;
volatile uint8_t tr_bench_mode;
volatile uint8_t tr_bench_lane;

#if TR_INPUT_NPU
/*
 * TR_INPUT_NPU: the body box comes from the HP's pose slot (tr_pslot.h),
 * not a local camera + classical detector -- design sec 5. tr_pslot_read()
 * itself already rejects a torn copy and an unpublished slot (magic/version);
 * this adds the "stale" half of design sec 3: a pose whose seq has not moved
 * in TR_NPU_STALE_TICKS consecutive calls (the HP pipeline stalled, or was
 * never resident) is treated the same as "nothing detected this tick", not
 * as a hard error -- capture_box() always returns a tr_box_t, valid or not,
 * exactly as the classical path does, so every caller (the calibration
 * loop, the main tick, fall_back()'s TR_CALIB_TIMEOUT_TICKS give-up) needs
 * no changes at all.
 */
static tr_box_t capture_box(void)
{
	tr_pslot_t out;
	uint32_t   seq;

	if (tr_pslot_read(g_pslot, g_pslot_seq, &out, &seq, pslot_barrier)) {
		g_pslot_seq   = seq;
		g_pslot_stale = 0u;
		g_pslot_last  = out; /* only replace the last-accepted copy on a real acceptance */
	} else if (g_pslot_stale <= TR_NPU_STALE_TICKS) {
		g_pslot_stale++;
	}

	if (g_pslot_stale > TR_NPU_STALE_TICKS || g_pslot_last.hp_state != TR_HP_STATE_RUNNING) {
		return (tr_box_t){ .valid = false };
	}

	/* Tagged with its seq: until the HP publishes again, every tick re-reads
	 * this same pose, and the tracker must count it once (track.h). */
	tr_box_t b = tr_pose_box_mirrored(&g_pslot_last.pose, hp_mirrored());

	b.seq = g_pslot_seq;
	return b;
}

/*
 * fix round 7 (silicon finding on 2c45a9c): the "camera unresponsive"
 * watchdog in main()'s loop (dead_ticks) exists to detect a DEAD
 * pipeline, not an EMPTY booth -- fall_back()'s own header comment already
 * says so ("assume the CAMERA has stopped, not that the player walked
 * away"). Before this fix they counted consecutive ticks of "no CALIBRATED
 * box", which an empty booth produces just as reliably as a dead HP: on
 * 2026W36-0009, with nobody standing in front of the camera, hp_state stayed
 * RUNNING and the pslot kept publishing fresh empty-pose frames the whole
 * time, yet the watchdog still fired at exactly 450 ticks and printed
 * "hp_state=0 (RUNNING) stale=0" right next to its own "unresponsive"
 * verdict -- proof the counter was never asking the right question. The
 * right liveness signal for TR_INPUT_NPU is the one capture_box() already
 * uses to accept a slot at all: hp_state == RUNNING and the slot not
 * stale. tr_camera_ok() below is that same check, exposed so the
 * watchdog can reset on it instead of on "got a usable box" -- it
 * measures "is the HP pipeline alive", the thing they are named
 * for, not "is anyone standing there" (fall_back() -- and this whole
 * module -- deliberately did not do that job before the pose slot
 * existed; it does now, so it should).
 */
static bool tr_camera_ok(void)
{
	return g_pslot_stale == 0u && g_pslot_last.hp_state == TR_HP_STATE_RUNNING;
}
#else
/*
 * Acquire one camera frame, detect the player, and release the frame on
 * every path -- the invariant Task 8's review called out: an unreleased
 * frame stalls the camera permanently. An invalid box means either no frame
 * was ready this tick (not an error: the camera runs on its own cadence) or
 * nothing was detected in the frame that was ready.
 */
static bool g_cam_frame; /* the last capture_box() got a frame: tr_camera_ok() */

static tr_box_t capture_box(void)
{
	size_t         len;
	const uint8_t *frame = tr_camera_frame(&len);

	g_cam_frame = frame != NULL;
	if (frame == NULL) {
		return (tr_box_t){ .valid = false };
	}

	tr_box_t b = tr_detect_frame(&g_detect, frame, len);

	tr_camera_release();
	b.h = 0; /* a whole-body box has no torso: track.c takes its width as the scale (track.h) */
	return b;
}

/* The classical path's liveness signal: frames arriving at all. A stopped
 * camera's NULL streak outlasts any camera cadence; an empty booth still
 * delivers frames (nothing detected in them) and never trips it. */
static bool tr_camera_ok(void)
{
	return g_cam_frame;
}
#endif

/* Whether a player is here (vision/pose.h tr_presence_step(): a torso on
 * TR_PRESENT_MIN of the last TR_PRESENT_WIN poses, never one box) -- what
 * attract, the join lobby, a run's end and the initials all decide on. The
 * torso is only the bar to join; in a run any confident box keeps them. */
static tr_presence_t g_presence;
static bool          g_present;

/* capture_box() plus the presence update: every vision read goes through
 * here, so presence is current wherever a box is. `joining`: attract is up,
 * so presence is the torso bar to join (the pose path; the classical box has
 * no torso to ask for). */
static tr_box_t sense(bool joining)
{
	tr_box_t b = capture_box();

	g_present = tr_presence_step(&g_presence, b, TR_INPUT_NPU && joining);
	return b;
}

/*
 * Shared with both the calibration retry loop and the in-run pause: past
 * TR_CALIB_TIMEOUT_TICKS consecutive ticks of "not getting a usable box",
 * assume the CAMERA has stopped, not that the player walked away, and fall
 * back to TR_FALLBACK_MODE (mode.h) so the run stays playable. `waited_ticks`
 * is only for the printed line.
 *
 * Whole-branch review F2: the calibration loop already had this bound; the
 * in-run pause (tr_ctl_step()'s `player_lost` path, mode.c) did not, and a
 * camera that stops delivering frames mid-run -- a permanent NULL from
 * tr_camera_frame(), indistinguishable from "the player walked away" -- used
 * to pin the game on "STEP BACK INTO VIEW" forever, with no player action
 * able to recover it.
 *
 * This is a DIFFERENT, harder failure than the reversible attract sub-state
 * (game/attract.h) that now also engages, much sooner (TR_ATTRACT_ENTER_TICKS,
 * a few seconds) and WITHOUT closing the camera, whenever nobody is simply
 * standing in front of it -- that path can always resume the instant a real
 * player shows up. This one only fires if that has not been enough to shake
 * loose a usable box for TR_CALIB_TIMEOUT_TICKS (~15 s): at that point the
 * camera itself is assumed broken, not just quiet, and this closes it for
 * good this run.
 */
static void fall_back(tr_mode_t    *mode,
                      tr_ctl_t     *ctl,
                      tr_track_t  **track_ptr,
                      tr_attract_t *attract,
                      uint32_t      waited_ticks,
                      bool          imu_ok)
{
	printk("camera unresponsive after %u ticks (~%u ms): falling back to %s mode\n",
	       waited_ticks,
	       waited_ticks * TICK_MS,
	       (TR_FALLBACK_MODE == TR_MODE_TILT) ? "tilt" : "attract");
#if TR_INPUT_NPU
	/* fix round 6: the generic message above says only "no usable box",
	 * which is one hop removed from the real cause when the HP publishes
	 * every tick but never reaches RUNNING (a stuck hp_state) -- exactly
	 * what a memmap layout mismatch between mismatched HE/HP builds looks
	 * like (tr_memmap.h TR_MEM_SRAM1_READY's own comment). Printing the
	 * pslot's last-accepted hp_state turns that class of bug into a
	 * one-line diagnosis instead of a reverse-engineering exercise. */
	printk("  pose slot last hp_state=%u (0=RUNNING, 1=NO_CAMERA, 2=I2C_STUCK, 3=NO_FRAME, "
	       "4=SRAM1_NOT_READY), stale=%u ticks\n",
	       g_pslot_last.hp_state,
	       g_pslot_stale);
#endif
	if (TR_FALLBACK_MODE == TR_MODE_TILT && !imu_ok) {
		printk("WARNING : IMU unavailable too -- tilt mode has no input source this run\n");
	}
	ui_banner(TR_BANNER_CHECK_CAMERA);
	ui_present(&g_idle, false, false);
	ui_hold(TR_FALLBACK_BANNER_MS);
#if !TR_INPUT_NPU
	/* Nothing will call capture_box() again this run: stop the CPI and free
	 * it -- see camera.h's doc comment for this exact situation. TR_INPUT_NPU:
	 * no local camera to close -- the HP keeps running (and its stale pose is
	 * simply never read again by this core). */
	tr_camera_close();
#endif
	*mode      = TR_FALLBACK_MODE;
	*track_ptr = NULL;
	ui_invite(*mode);
	tr_ctl_init(ctl, *mode);
	/* Fresh synthetic-AI state regardless of target: harmless when the
	 * target is tilt (nothing reads it there), and required when it is
	 * attract -- this call site's `attract` may already carry state left
	 * over from the reversible in-vision sub-state. */
	tr_attract_init(attract);
}

/*
 * Booth: a player run that makes the high-score table (game/hiscore.h).
 * With the HUD layer to show it, the player picks three letters on the
 * frozen last crash frame -- by tilting the board (tr_ini_tilt_frame(): a
 * walk-away here also sends a takeover back to attract, tr_tilt_run_over())
 * or, in a camera build, by the tracked body; otherwise the letters stand
 * as they start, the character's name. RAM only.
 */
static void enter_high_score(tr_game_t *g, tr_mode_t mode, tr_track_t *track, bool imu_ok)
{
	int           rank = tr_hs_rank(&g_hs, g_score.score);
	uint8_t       src  = tr_hs_entry_input(ui_hud_up(), imu_ok, mode);
	tr_initials_t e;

	if (rank < 0) {
		return;
	}
	if (track != NULL) {
		tr_track_resync(track); /* an arm still up from the run must not step the initials */
	}
	tr_ini_start(&e, ui_default_name(), rank);
	if (src == TR_HS_IN_VISION && track == NULL) {
		src = TR_HS_IN_NONE; /* the camera has been given up on */
	}
	g_ini = &e;
	for (bool more = src != TR_HS_IN_NONE; more;) {
		if (src == TR_HS_IN_TILT) {
			int16_t tx, ty;

			tr_imu_read_q8(&tx, &ty);
			more = tr_ini_tilt_frame(&e, &tr_tilt, tx, ty);
		} else {
			more = tr_ini_vision_frame(&e, track, sense(false), g_present);
		}
		if (more) {
			ui_present(g, false, false);
		}
	}
	g_ini = NULL;
	(void)tr_hs_insert(&g_hs, g_score.score, e.name);
	printk("hiscore : #%d %s %u (%s)\n",
	       rank + 1,
	       e.name,
	       g_score.score,
	       e.why == TR_INI_DONE       ? "entered"
	       : e.why == TR_INI_ENTERING ? "default"
	                                  : "auto-filled");
}

/* A give-up: stay here, main's stack and every object on it alive, the
 * console readable. Returning from main() instead ends the main thread,
 * and that exit path is where the fail runs hard-faulted in SysTick
 * (bench, 2026W36-0009: MPU fault, pc 0xaaaaaaaa, r3 -> the "main" thread-name
 * string). */
static FUNC_NORETURN void park(void)
{
	for (;;) {
		k_sleep(K_FOREVER);
	}
}

int main(void)
{
	printk("\n=== trace-runner ===\n");

	/*
	 * display.c's own comment states the panel's init fails on roughly 1
	 * cold boot in 8-10, and says "power-cycle and retry" -- with
	 * CONFIG_UART_CONSOLE=n that used to mean a silent black screen and
	 * nothing else on a filmed demo's first take, with no operator-visible
	 * tell that it was a known, recoverable panel defect rather than a
	 * hang (whole-branch review F13). A software retry cannot power-cycle
	 * the panel; the failure is plausibly a power-up timing race, and a
	 * handful of attempts a short beat apart costs little, but the
	 * attempts are not independent samples (same board, same rail, 200 ms
	 * apart), so no probability is claimed here -- only that retrying is
	 * cheap and the intermittent path is the one known to sometimes
	 * recover on its own.
	 *
	 * Only tr_display_open()'s -1 return (the intermittent
	 * alp_display_open()-returned-NULL path) is retried. Its -2 return is
	 * a genuine, deterministic configuration failure (caps unavailable, or
	 * a pixel format this project cannot draw to) that retrying cannot
	 * fix -- see tr_display_open()'s comment. Retrying that blindly used
	 * to leak a handle from the SDK's fixed display-handle pool on every
	 * attempt and, once the pool was exhausted, misreport the eventual
	 * NOMEM failure as the intermittent panel defect (whole-branch fix
	 * round B, B1).
	 */
	/* The panel is up before main(): the SDK brings the HX8394 up with retries
	 * (src/zephyr/panel_init_retry.c) and the RVT121's bridge configures
	 * itself, both at APPLICATION init. */

	bool display_ok  = false;
	int  open_result = -1;

	for (int attempt = 0; attempt < TR_DISPLAY_OPEN_RETRIES; attempt++) {
		open_result = tr_display_open();
		if (open_result == 0) {
			display_ok = true;
			break;
		}
		if (open_result != -1) {
			break; /* Deterministic failure: tr_display_open() already printed why. */
		}
		if (attempt + 1 < TR_DISPLAY_OPEN_RETRIES) {
			/* Deliberately not the "RESULT FAIL:" bench-log token (grepped
			 * for by camera.c too) -- this boot may yet succeed, and that
			 * token must mean "gave up", not "one attempt failed". */
			printk(
			    "display : retrying open (attempt %d/%d)\n", attempt + 2, TR_DISPLAY_OPEN_RETRIES);
			k_msleep(TR_DISPLAY_RETRY_MS);
		}
	}
	if (!display_ok) {
		if (open_result == -1) {
			/* Every attempt hit the intermittent path: this is a real
			 * give-up, so this is the one place this loop earns the
			 * terminal token. */
			printk("RESULT FAIL: display did not open after %d attempts -- giving up\n",
			       TR_DISPLAY_OPEN_RETRIES);
		}
		/* open_result == -2: tr_display_open() already printed its own
		 * RESULT FAIL: line for the real failure; nothing to add. */
		park();
	}
#if TR_RENDER_A32
	tr_a32_boot();
	(void)tr_hud_l2_open(); /* false: no layer 2, the A32 keeps its sprite HUD */
#endif
	sfx_init();
	tr_score_init(&g_score);
	tr_hs_init(&g_hs);
	ui_reset();
	/* The first flip: it puts the just-painted background on the glass AND
	 * brings the other buffer level with it (display.h), so neither can ever
	 * show uninitialised SRAM. Every picture below owes the same call. */
	ui_present(&g_idle, false, false);
	/* Tilt is optional -- a missing/dead IMU just leaves neutral intents in
	 * tilt mode -- but the result still matters: if the camera ALSO fails,
	 * imu_ok tells the checks below whether tilt is a real fallback or a
	 * mode with no input source at all (whole-branch review F4 point 3). */
	bool imu_ok = (tr_imu_open() == 0);
	(void)tr_rail5v_open(); /* +5V power HUD line -- non-fatal, see rail5v_power.h */
	tr_bus2_he_arm();       /* TR_HP_SOUND: only now may the HP be offered I2C2 */

#if TR_INPUT_NPU
	/* No local camera or detector to open/init: the HP owns both, and
	 * capture_box() already treats "the HP has never published, or its
	 * pose has gone stale" as an invalid box, the same shape as "no frame
	 * ready this tick" on the classical path. So there is nothing here to
	 * fail at start-of-day -- vision mode is always attempted, and the
	 * existing TR_CALIB_TIMEOUT_TICKS give-up (fall_back(), below) is what
	 * demotes to TR_FALLBACK_MODE if the HP image never comes up at all. */
	bool camera_ok = true;
	bool detect_ok = true;
#else
	/* Mode selection: camera open AND detector init must both succeed to
	 * play in vision mode -- camera-open-without-a-fitting-detector-geometry
	 * is not enough (see detect.h's tr_detect_init() contract). Any other
	 * outcome falls back to TR_FALLBACK_MODE. */
	bool camera_ok = (tr_camera_open() == 0);
	bool detect_ok =
	    camera_ok &&
	    (tr_detect_init(&g_detect, tr_camera_width(), tr_camera_height(), TR_CAM_DECIM) == 0);

	if (camera_ok && !detect_ok) {
		/* Nothing will ever call capture_box() in this run: stop the CPI and
		 * free it instead of leaving it streaming into the video pool for
		 * nothing (SDK gap: no way to ask for the frames without starting
		 * the stream in the first place -- see task-9-report.md). */
		tr_camera_close();
	}
#endif

	tr_mode_t mode = tr_ctl_select_mode(camera_ok, detect_ok);

	ui_invite(mode);

	/* Name all three modes: the ternary here used to print "TILT" for
	 * anything that was not VISION, which would now misreport an ATTRACT
	 * fallback as tilt -- and this console line is the only way to tell
	 * which mode a bench run actually chose. */
	printk("mode    : %s (camera_ok=%d detect_ok=%d imu_ok=%d)\n",
	       (mode == TR_MODE_VISION)    ? "VISION"
	       : (mode == TR_MODE_ATTRACT) ? "ATTRACT"
	                                   : "TILT",
	       (int)camera_ok,
	       (int)detect_ok,
	       (int)imu_ok);
	if (mode == TR_MODE_TILT && !imu_ok) {
		printk("WARNING : camera unusable and IMU unavailable -- no input source this run\n");
	}

	tr_ctl_t     ctl;
	tr_track_t   track;
	tr_track_t  *track_ptr = (mode == TR_MODE_VISION) ? &track : NULL;
	tr_attract_t attract; /* the reversible in-vision sub-state; also reused, reinitialised,
				* by fall_back() for the standalone TR_MODE_ATTRACT fallback. */

	tr_ctl_init(&ctl, mode);
	tr_attract_init(&attract);
	tr_tilt_init(&tr_tilt);

	if (mode == TR_MODE_VISION) {
		/* Nobody has joined yet: boot into attract. A player joins through
		 * the lobby (tr_attract_step(): present TR_ATTRACT_JOIN_TICKS under
		 * "STEP INTO VIEW", the tracker calibrating on them) -- never on a
		 * box seen at boot (silicon, 4612458: an empty room's flicker
		 * calibrated the old title screen and started a phantom run). */
		tr_track_init(&track, tr_camera_height());
		tr_attract_enter(&attract);
		printk("step into view...\n");
	}

	tr_game_t g;

	run_start(&g);
	tr_lobby_init(&g_lobby);
	tr_zone_reset(&g_zone);

	bool     was_paused = false;
	uint32_t dead_ticks = 0; /* Consecutive ticks the camera looked dead -- see fall_back() (F2). */
	uint32_t run_count =
	    0; /* Printed every TR_RUN_OVER_PRINT_EVERY runs -- see the death branch. */
	tr_intent_t held = tr_intent_none(); /* input gathered since the last paced step */

	for (;;) {
		int64_t  start  = k_uptime_get();
		uint64_t t_tick = k_cycle_get_64();

		tr_bus2_he_frame(); /* TR_HP_SOUND: offer / take back I2C2 for the HP's amp bring-up */
		tr_rail5v_poll();   /* internally paced to ~3 Hz -- see rail5v_power.c */

		tr_intent_t in          = tr_intent_none();
		bool        pace_step   = true; /* false on the frames between paced game steps */
		bool        player_lost = false;

		if (mode == TR_MODE_VISION) {
			tr_box_t b = sense(attract.active);

			in          = tr_track_update(&track, b);
			player_lost = tr_track_player_lost(&track);
			if (attract.active && g_present) {
				tr_track_calibrate(&track, b); /* the join lobby: on them */
			}
		} else {
			int16_t tx, ty;

			tr_imu_read_q8(&tx, &ty);
			if (mode == TR_MODE_TILT) {
				in = tr_tilt_intent(&tr_tilt, tx, ty);
			} else if (tr_tilt_step(&tr_tilt, tx, ty, &in) != TR_TILT_STAY) {
				/* TR_MODE_ATTRACT, no camera: the IMU is the body input.
				 * A deliberate tilt took over, or the player walked away
				 * -- either way a fresh run (game/tilt.h); a takeover
				 * ends the lobby at once, a walk-away opens it (P16). */
				run_start(&g);
				tr_zone_reset(&g_zone);
				tr_ctl_reset(&ctl, track_ptr);
				ui_reset();
				held = tr_intent_none(); /* nothing from before the takeover / walk-away */
			}
			if (mode == TR_MODE_ATTRACT && !tr_tilt.playing) {
				pace_step = tr_game_pace(&g_phase_q16, tr_ramp_frame_q16(true, g.tick));
				if (pace_step) {
					in = tr_attract_intent(&attract, &g, (int16_t)tr_display_height());
				}
			}
		}

		bool run_step;

		if (mode == TR_MODE_VISION && attract.active) {
			/* Driving itself: bypass tr_ctl_step()'s own pause/resync
			 * bookkeeping entirely while active -- tr_attract_step() is
			 * the sole authority on the tracker and the run this tick, so
			 * routing through both would double up its leaving-attract
			 * resync with tr_ctl_step()'s own leaving-pause one. */
			if (tr_attract_step(&attract, &track, g_present) == TR_ATTRACT_LEFT) {
				/* A real player just took the idle board back over --
				 * abandon whatever the synthetic run was doing and start
				 * fresh for them, exactly like any other run-reset (see
				 * mode.h's tr_ctl_reset() doc). */
				run_start(&g);
				tr_zone_reset(&g_zone);
				tr_ctl_reset(&ctl, track_ptr);
				ui_reset();
				was_paused = false;
				run_step   = false; /* this tick is spent on the transition, not a step */
				held       = tr_intent_none();
			} else {
				pace_step = tr_game_pace(&g_phase_q16, tr_ramp_frame_q16(true, g.tick));
				if (pace_step) {
					in = tr_attract_intent(&attract, &g, (int16_t)tr_display_height());
				}
				run_step = true;
			}
		} else {
			run_step = tr_ctl_step(&ctl, track_ptr, player_lost);
			if (mode == TR_MODE_VISION &&
			    tr_attract_step(&attract, &track, g_present) == TR_ATTRACT_ENTERED) {
				/* Nobody for TR_ATTRACT_ENTER_TICKS: the run ENDS here --
				 * attract plays demo runs, never the walked-off player's
				 * (the best still counts). */
				tr_score_run_end(&g_score, true);
				run_start(&g);
				tr_ctl_reset(&ctl, track_ptr);
				ui_reset();
				run_step = false;
				held     = tr_intent_none();
			}
		}

		bool now_paused = (mode == TR_MODE_VISION) && ctl.paused && !attract.active;

		if (mode == TR_MODE_VISION) {
#if TR_INPUT_NPU
			/* fix round 7: the liveness question (is the HP publishing),
			 * not "is anyone there" -- a healthy HP resets this every tick,
			 * in a run and in attract alike (a vision boot starts in
			 * attract now, so this is also the old calibration loop's
			 * never-came-up bound). */
			dead_ticks = tr_watchdog_tick(dead_ticks, tr_camera_ok());
#else
			/* In a run, a pause that never ends is the camera's (F2). A
			 * vision boot starts in attract, which never pauses: there a
			 * frameless streak is, so a dead camera still falls back. */
			dead_ticks = now_paused       ? dead_ticks + 1u
			             : attract.active ? tr_watchdog_tick(dead_ticks, tr_camera_ok())
			                              : 0u;
#endif
		}
		if (mode == TR_MODE_VISION && dead_ticks >= TR_CALIB_TIMEOUT_TICKS) {
			/* A camera that has stopped producing frames looks identical
			 * to a player who walked away -- bound it, or this never
			 * recovers. */
			fall_back(&mode, &ctl, &track_ptr, &attract, dead_ticks, imu_ok);
			dead_ticks = 0;
			now_paused = false; /* mode is TR_FALLBACK_MODE now: nothing left to pause. */
			ui_reset();
		} else if (now_paused) {
			ui_banner(TR_BANNER_STEP_BACK);
		} else if (was_paused) {
			/* Leaving pause: wipe the banner once, then let the normal
			 * erase/draw pass in tr_render_frame() take back over. */
			ui_reset();
		}
		was_paused = now_paused;

		uint64_t t_render   = k_cycle_get_64();
		bool     attract_on = (mode == TR_MODE_VISION && attract.active) ||
		                      (mode == TR_MODE_ATTRACT && !tr_tilt.playing);
		bool     joining    = mode == TR_MODE_VISION && tr_attract_joining(&attract);
		/* Someone is there (any confident box on the window) but without
		 * the torso to join -- hips out of frame, too close: tell them.
		 * Held a few ticks first: walking in, the any-box window fills
		 * one pose before the torso window, which would flash the hint
		 * for a frame ahead of STAND. */
		static uint8_t step_back_hold;
		bool step_back_now = TR_INPUT_NPU && mode == TR_MODE_VISION && attract.active && !joining &&
		                     !g_present && tr_presence_is(&g_presence, false);
		step_back_hold =
		    step_back_now
		        ? (step_back_hold < TR_STEP_BACK_HOLD_TICKS ? step_back_hold + 1u : step_back_hold)
		        : 0u;
		bool step_back = step_back_hold >= TR_STEP_BACK_HOLD_TICKS;

		/* The lobby (P16): standing, the world does not step and shows
		 * no sub-tick motion (an attract intent rolled this frame is
		 * dropped: a fresh run has nothing in reach to spend it on). */
		if (tr_lobby_frame(&g_lobby, TR_RENDER_A32 && attract_on)) {
			pace_step   = false;
			g_phase_q16 = 0u;
		}

		if (!attract_on && run_step) {
			/* Play at the game pace (state.h TR_GAME_PACE_Q8); the
			 * player's input is held over the frames between steps. */
			tr_intent_t step_in = tr_intent_none();

			pace_step = tr_play_frame(
			    &g, &held, in, &g_phase_q16, tr_ramp_frame_q16(false, g.tick), &step_in);
			in = step_in; /* lane moves already applied this frame (state.h) */
		} else if (!run_step) {
			held = tr_intent_none(); /* paused / resyncing: nothing carries over */
		}
		g_pace_q8 = tr_ramp_pace_q8(tr_ramp_frame_q16(attract_on, g.tick));

		if (run_step) {
			/* Paced (play TR_PLAY_SPEED_Q16, attract TR_ATTRACT_SPEED_Q16):
			 * a frame between two game steps re-presents the same state
			 * at a later g_phase_q16, which the A32 interpolates. */
			if (pace_step) {
				tr_game_step(&g, in, (int16_t)tr_display_height());
				tr_score_step(&g_score, &g);
				tr_react_step(&g_react, &g, g_score.combo); /* P16: what the runner reacts to */
				if (g.alive) { /* the world stands still through a crash */
					g_zone.ramp_q8 = tr_ramp_stepped_q8(g.tick); /* the speed it stepped at */
					(void)tr_zone_step(&g_zone, attract_on, (int16_t)tr_display_height());
				}
			}
			ui_frame(&g);
		}

		if (attract_on) {
			/* The A32 build shows the attract screen on layer 2
			 * (hud.c: logo, tagline, TILT TO PLAY / STEP IN TO PLAY
			 * per ui_invite()); the sprite banner this id also
			 * selects says STEP IN TO PLAY, and only the M55 build
			 * (or an A32 build without layer 2) still shows it. */
			/* Self-playing, either flavour -- drawn over the live frame
			 * just rendered above, the same way the game-over banner below
			 * draws over a frozen one (F7): if this tick's synthetic run
			 * just died, that banner paints over this one, which is the
			 * right thing to show at the moment of death either way. */
			/* The join lobby: the demo plays on under "STEP INTO VIEW"
			 * (the HUD's banner card, not the attract screen) until the
			 * player has stood there TR_ATTRACT_JOIN_TICKS. */
			ui_banner(joining     ? TR_BANNER_STAND
			          : step_back ? TR_BANNER_STEP_BACK
			                      : TR_BANNER_ATTRACT);
		}

		/*
		 * ONE flip per tick, after every drawing path above (the pause
		 * banner, tr_render_init()'s repaints, the frame itself, the
		 * attract banner) -- so the whole tick's picture appears at once,
		 * and the erase pass inside tr_render_frame() happens in a buffer
		 * the CDC200 is not scanning out. Flipping per draw instead would
		 * cost one vertical-blanking wait each (~25.0 ms at the panel's
		 * 40.0 Hz) and blow TICK_MS.
		 */
		tr_cyc_render += k_cycle_get_64() - t_render;
		ui_present(&g, attract_on && !joining && !step_back, now_paused);
		sfx_frame(&g, attract_on);
		tr_cyc_work += k_cycle_get_64() - t_tick;
		tr_ticks++;

		if (!g.alive) {
			/*
			 * Throttled to one line per TR_RUN_OVER_PRINT_EVERY runs (and
			 * the periodic fps line removed outright) -- the 8,191-byte RAM
			 * console (the only diagnostic channel this board has,
			 * CONFIG_UART_CONSOLE=n) wraps with no wrap marker, and the
			 * one-shot boot/mode/calibration-timeout lines that actually
			 * explain a bad demo are worth more than a running score
			 * (whole-branch review F6).
			 */
			/* Commit the run to the session best -- a demo run never counts. */
			tr_score_run_end(&g_score, !attract_on);
			if (++run_count % TR_RUN_OVER_PRINT_EVERY == 0u) {
				printk("run over: score=%u best=%u ticks=%u (run #%u)\n",
				       g_score.score,
				       g_score.best,
				       g.tick,
				       run_count);
			}
			/*
			 * F7 (whole-branch review): death used to freeze on the last
			 * game frame for 1.5 s with no indication. The banner is the
			 * "you died" signal; the frame above already carries the
			 * final score. P6: in the A32 build the crash then plays out
			 * as ~1.5 s of live frames (ui_crash) instead of a frozen
			 * one -- the obstacle visibly stopped at the runner.
			 */
			ui_crash(&g, attract_on);
			if (!attract_on) {
				sfx_push(TR_AEV_GAME_OVER, 0); /* the crash has played out */
				enter_high_score(&g, mode, track_ptr, imu_ok);
			}
			if (mode == TR_MODE_ATTRACT && tr_tilt.playing) {
				(void)tr_tilt_run_over(&tr_tilt); /* idle player: back to attract */
			}
			run_start(&g);
			if (attract_on) { /* a demo run over: the next character, the lobby again */
				tr_tilt_demo_over(&tr_tilt);
				tr_lobby_demo_over(&g_lobby);
			} else {
				tr_zone_reset(&g_zone); /* a player's next run starts on the board */
				if (mode == TR_MODE_VISION) {
					/* Nobody there any more (walked off, or the initials
					 * idled out): attract, not a new run. */
					(void)tr_attract_run_over(&attract, g_present);
				}
			}
			tr_ctl_reset(&ctl, track_ptr); /* run-reset resync: see mode.h */
			ui_reset();
			was_paused = false;
			held       = tr_intent_none(); /* nothing carried into the next run */
		}

		tr_bench_mode = (uint8_t)mode;
		tr_bench_lane = g.lane;

		int64_t spent = k_uptime_get() - start;

		if (spent < TICK_MS) {
			tick_sleep((int32_t)(TICK_MS - spent));
		}
	}
	return 0;
}
