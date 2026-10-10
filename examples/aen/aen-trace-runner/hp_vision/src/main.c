/* hp_vision/src/main.c -- M55-HP vision app: OV9281 capture -> newest-frame
 * -> letterbox 192x192 -> Ethos-U55-256 cut MoveNet -> integer decode ->
 * pose slot (src/ipc/tr_pslot.h), for the HE's TR_INPUT_NPU build to read.
 * Design: docs/superpowers/specs/2026-09-24-npu-body-control-design.md.
 *
 * Production successor to two proven pieces: probe/npu (the NPU op alone,
 * canned frames, no camera) and probe/camera (the OV9281 alone, on the HE).
 * This app reuses ../src/platform/camera.c verbatim (the SAME <alp/camera.h>
 * portable path aen-camera-firstlight proved at 101 fps on 2026W36-0009) and
 * ../src/vision/{movenet,pose,camera_ae}.c + ../src/ipc/tr_pslot.c, the
 * host-tested pure logic -- this file is the glue: I2C1 unstick, the
 * capture/AE/inference/publish loop, and kernel-cycle timing into a bench-readable
 * debug block.
 *
 * UNPROVEN ON SILICON: the CSI/CPI devicetree path on rtss_hp (design sec 9
 * item 3) links clean (boards/shields/tr_hp_csi_complete/, a local ordering
 * fix -- see that dir's header) but camera capture itself has only been
 * silicon-proven on the HE (PR #2247, and today's 2026W36-0009 bring-up, both
 * M55-HE).
 */
#include <string.h>

#include <cmsis_core.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/video-controls.h>
#include <zephyr/drivers/video.h>

#include <alp/inference.h>
#include <alp/peripheral.h>

#include "../../src/ipc/tr_hp_dbg.h"
#include "../../src/ipc/tr_cam_view.h"
#include "../../src/ipc/tr_memmap.h"
#include "../../src/ipc/tr_pslot.h"
#include "../../src/platform/camera.h"
#include "../../src/vision/cam_rot.h"
#include "../../src/vision/camera_ae.h"
#include "../../src/vision/kp_smooth.h"
#include "../../src/vision/movenet.h"
#include "../../src/vision/movenet_mram.h"
#include "../../src/vision/pose.h"

/* The board overlay's side of the I2C1 handover names the same flag words the HE's does. */
BUILD_ASSERT(DT_PROP(DT_NODELABEL(i2c1_handover), flag_address) == TR_MEM_I2C1_HANDOVER,
             "i2c1_handover flag-address != tr_memmap.h TR_MEM_I2C1_HANDOVER");

/* ---- I2C1 SCCB unstick (task facts, silicon-proven on 2026W36-0009 today) --
 * I2C1 (0x49011000, the camera SCCB bus, shared with GT911 touch at 0x14)
 * latches stuck after boot: every transfer times out with rc -116. Cure:
 * before the first transfer, force the pad function back to GPIO (0) and
 * then to I2C1 (5) again on both pins -- the brief function-0 excursion
 * frees a slave stuck stretching the clock, the same class of fix as an I2C
 * bit-bang bus-recovery (9 clocks + STOP), but at the pad-mux level rather
 * than bit-banging SCL. Raw MMIO, not the pinctrl API: this is a runtime
 * recovery kick, not a boot-time pin configuration (pinctrl already ran and
 * set these same pins to function 5 before main() -- this repeats it after
 * deliberately glitching through function 0). */
#define TR_I2C1_PAD_P3_7 0x1A60307Cu
#define TR_I2C1_PAD_P7_2 0x1A6030E8u
#define TR_I2C1_PAD_GPIO 0x00290000u /* function 0 */
#define TR_I2C1_PAD_I2C1 0x00290005u /* function 5 -- I2C1_SCL_B / I2C1_SDA_C */

/*
 * Reviewer finding, fix round 1: calling this from main() is too late --
 * Zephyr's own device init (which runs the ov9281 driver's chip-ID read,
 * itself needing the I2C1 controller already initialised) happens entirely
 * BEFORE main() ever runs. If I2C1 was left stuck from a prior boot, the
 * sensor's own init fails first and the camera never comes up, no matter
 * what main() does afterward. Fixed with SYS_INIT at POST_KERNEL priority
 * 1 -- ahead of the i2c_dw instance (POST_KERNEL priority 40) and the sensor
 * driver after it, and behind the HE handover wait (priority 0, see below). This works with NO devicetree change (no
 * zephyr,deferred-init) because the unstick is raw MMIO on the pad control
 * registers directly, independent of whatever init order pinctrl or the
 * i2c1 controller driver use -- it does not need either to have run first.
 */
static int tr_i2c1_unstick_init(void)
{
	sys_write32(TR_I2C1_PAD_GPIO, TR_I2C1_PAD_P3_7);
	sys_write32(TR_I2C1_PAD_GPIO, TR_I2C1_PAD_P7_2);
	sys_write32(TR_I2C1_PAD_I2C1, TR_I2C1_PAD_P3_7);
	sys_write32(TR_I2C1_PAD_I2C1, TR_I2C1_PAD_P7_2);
	return 0;
}
/* POST_KERNEL priority 1, after the alp,i2c-handover wait (priority 0, boards/<board>.overlay:
 * the HE may be using this bus for a display bridge until it releases it) and
 * still ahead of every driver (the i2c_dw instance is POST_KERNEL priority 40). */
SYS_INIT(tr_i2c1_unstick_init, POST_KERNEL, 1);

/* ---- software AE: apply tr_ae_step()'s result directly over I2C to the
 * sensor's own AE registers (chips/ov9281/zephyr/drivers/video/ov9281.c
 * documents the register map; fix round 13: no longer through the Zephyr
 * video control API at all, see apply_ae()'s own header comment for why)
 * -- an SDK GAP, the same shape as src/platform/display.c's cdc200_swap_fb()
 * bypass: <alp/camera.h> has no exposure/gain control, so this reaches the
 * sensor directly. ov9281@60 is on E1M I2C1 (csi_i2c), from alp-sdk-lcd's
 * innomaker_cam_ov9281 shield -- probe/camera/src/main.c's SENSOR_NODE.
 * g_sensor itself is still needed for device_is_ready() (the driver's own
 * init, and the camera pipeline's use of the SAME device elsewhere in this
 * file), even though nothing here calls video_set_ctrl() on it any more. */
#define SENSOR_NODE DT_NODELABEL(ov9281)
static const struct device *const g_sensor = DEVICE_DT_GET(SENSOR_NODE);
/* Independent i2c_dt_spec for the SAME physical sensor (SENSOR_NODE's own
 * devicetree bus + reg=<0x60>) -- the actual I2C handle every raw register
 * read/write in this file goes through (VTS, AE). video_read_cci_reg()/
 * video_write_cci_reg() (the helpers the driver itself uses) live in a
 * Zephyr-internal drivers/video/video_common.h, not a public include -- an
 * app can't reach them, so this is a plain, standard i2c_write_read_dt()/
 * i2c_write_dt() instead, same protocol (16-bit big-endian register
 * address, SCCB auto-increments across adjacent registers). */
static const struct i2c_dt_spec g_sensor_i2c = I2C_DT_SPEC_GET(SENSOR_NODE);

/* ---- bench-readable per-stage cycle timing + status: src/ipc/tr_hp_dbg.h
 * (fix round 5: moved from a local typedef here to a header shared with the
 * HE's HUD, src/platform/hud_l2.c -- the HUD had no canonical layout to
 * read this against before and showed the unrelated P10 sound-ring status
 * instead, always "M55-HP --"; see that header's own comment). Declared
 * here, ahead of apply_ae() below (fix round 12: apply_ae() needs both
 * this and pslot_barrier() for its own AE-register-readback seqlock; both
 * used to sit after apply_ae() in file order, fine when nothing above
 * them needed either). */
static volatile hp_dbg_t *const g_dbg = (volatile hp_dbg_t *)TR_MEM_HP_DBG;

static void pslot_barrier(void)
{
	__DSB();
}

/* chips/ov9281/zephyr/drivers/video/ov9281.c: OV9281_EXPOSURE_MIN 0x05,
 * OV9281_GAIN_DEFAULT_INDEX 8, ov9281_gain_reg_map[] 112 entries,
 * OV9281_EXP_MAX_OFFSET 25 (lines, the sensor's own minimum VTS-to-exposure
 * readout margin -- reused here, not re-derived, src/vision/camera_ae.h's
 * tr_ae_exposure_max_from_vts()).
 *
 * fix round 11 (silicon finding, raw frame mean 16.5/255, "far too dark"),
 * corrected fix round 13 (maintainer): TR_AE_EXP_MAX used to be a fixed
 * 0x2a9 (OV9281_EXPOSURE_DEFAULT) -- 681 WHOLE LINES, always a valid,
 * sensible exposure value (round 11 mistakenly believed the register was
 * in 1/16-line units and this decoded to "only 42 lines"; it does not --
 * src/vision/camera_ae.h's tr_ae_exposure_regs() has the real derivation).
 * The one genuine fix from round 11 is making the ceiling LIVE: VTS (AEC
 * registers 0x380E/0x380F) is genuinely per-mode (the driver's own table:
 * 100/50/100 fps, three different VTS values), read once tr_camera_open()
 * has configured the active mode (below), not assumed. g_ae_exp_max starts
 * at a SAFE FALLBACK (the known 640x400 @ 100 fps mode's own VTS, in case
 * the read ever fails) and is corrected to the sensor's live VTS the first
 * time the camera opens.
 *
 * Frame-rate/exposure-headroom trade-off (maintainer ruling, round 11):
 * halving the sensor rate (100 -> ~50 fps via a larger VTS) would roughly
 * double the line-time budget this ceiling computes over, more useful
 * exposure headroom for a dark booth. NOT done -- the maintainer kept
 * 100 fps (a booth's dark frame is partly a genuinely dark room, not purely
 * a ceiling bug). Left as a documented, NOT implemented, option: if booth
 * lighting still under-exposes, revisit lowering the rate (needs its own
 * live VTS *write*, not just this read, and re-verification that camera.c's
 * own frame-timing assumptions still hold at the new interval -- untested,
 * so not attempted blind here). */
#define TR_AE_EXP_MIN      0x05u
#define TR_AE_VTS_MARGIN   25u   /* OV9281_EXP_MAX_OFFSET */
#define TR_AE_VTS_FALLBACK 1096u /* OV9281_MODE_640X400_100FPS's own VTS */
#define TR_AE_GAIN_IDX_0   8u

/* TR_AE_VTS_FALLBACK's own ceiling, until tr_ae_refresh_exposure_ceiling()
 * (below) corrects it to the sensor's live VTS. */
static uint32_t g_ae_exp_max =
    TR_AE_VTS_FALLBACK - TR_AE_VTS_MARGIN; /* whole lines (polish round: the
									    * round-12 "<< 4" had survived here,
									    * a 17136-line fallback and an 8568-
									    * line first exposure) */

/* The AE state: tr_ae_refresh_exposure_ceiling() writes it to the sensor on
 * every (re)open, so the controller's idea of the exposure is the sensor's. */
static tr_ae_t g_ae;
static void    apply_ae(const tr_ae_t *ae);

/* AEC_EXPO's VTS registers, 0x380E (high byte) / 0x380F (low byte),
 * big-endian 16-bit register address + auto-incrementing SCCB read (the
 * SAME protocol chips/ov9281's own mode tables write VTS with, just read
 * instead of written, and independent of that driver's internals -- see
 * g_sensor_i2c's own comment). Returns 0 and *vts on success, a negative
 * errno otherwise (i2c_write_read_dt's own code) -- the caller keeps
 * TR_AE_VTS_FALLBACK on failure rather than deriving a ceiling from a
 * half-read/garbage VTS. */
static int tr_ae_read_vts(uint16_t *vts)
{
	uint8_t reg_addr[2] = { 0x38, 0x0E };
	uint8_t data[2];
	int     ret = i2c_write_read_dt(&g_sensor_i2c, reg_addr, sizeof(reg_addr), data, sizeof(data));

	if (ret < 0) {
		return ret;
	}
	*vts = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
	return 0;
}

/* Called once the camera has actually opened (tr_camera_open() == 0): the
 * active mode's registers, VTS included, are only programmed to the sensor
 * at that point (ov9281_set_fmt()'s video_write_cci_multiregs()), so a read
 * any earlier would race the sensor's own boot-default VTS, not the real
 * mode's. Leaves g_ae_exp_max at its current value (the fallback, the first
 * time this is called) on a failed read -- logged, not fatal: the fallback
 * is still a real ~25x improvement over the old fixed 42-line ceiling. */
static void tr_ae_refresh_exposure_ceiling(void)
{
	uint16_t vts;
	int      ret = tr_ae_read_vts(&vts);

	if (ret < 0) {
		printk(
		    "ae      : VTS read failed (%d) -- keeping exposure ceiling %u\n", ret, g_ae_exp_max);
		return;
	}
	g_ae_exp_max = tr_ae_exposure_max_from_vts(vts, TR_AE_VTS_MARGIN);
	printk("ae      : VTS %u -> exposure ceiling %u lines\n", vts, g_ae_exp_max);
}

/* 1 once the sensor's flip bit read back as TR_CAM_MIRROR asked
 * (cam_rot.h): published in tr_cam_view_t.mirror, so the descriptor tells
 * the truth even if the write never landed. */
static uint16_t g_cam_mirrored;

static int ov9281_read_reg8(uint16_t reg, uint8_t *val);
static int ov9281_write_reg8(uint16_t reg, uint8_t val);

/* After every camera (re)open -- ov9281_set_fmt()'s mode table rewrites the
 * register: set or clear the selfie-mirror flip bit (cam_rot.h
 * tr_cam_mirror_reg(), read-modify-write so the mode's binning bits stay),
 * then read it back. */
static void tr_cam_mirror_apply(void)
{
	uint16_t reg = tr_cam_mirror_reg(TR_CAM_ROTATE);
	uint8_t  v   = 0u;
	int      rc  = ov9281_read_reg8(reg, &v);

	if (rc >= 0) {
		v  = (uint8_t)((v & ~TR_OV9281_FLIP_BIT) | tr_cam_mirror_bit(TR_CAM_ROTATE, TR_CAM_MIRROR));
		rc = ov9281_write_reg8(reg, v);
	}
	if (rc >= 0) {
		rc = ov9281_read_reg8(reg, &v);
	}
	g_cam_mirrored = (uint16_t)(rc >= 0 && tr_cam_mirrored_from_reg(TR_CAM_ROTATE, v));
	printk("camera  : mirror %d -> 0x%04x = 0x%02x (rc %d)%s\n",
	       TR_CAM_MIRROR,
	       reg,
	       v,
	       rc,
	       rc >= 0 && (bool)g_cam_mirrored == (bool)TR_CAM_MIRROR ? "" : " -- MIRROR NOT APPLIED");
}

/* After every camera (re)open: clamp the AE state to the live ceiling and
 * write it, so the sensor holds exactly what the controller steps from (the
 * driver's mode table leaves its own default exposure otherwise), then let
 * one frame settle. */
static void tr_ae_camera_opened(void)
{
	tr_cam_mirror_apply();
	tr_ae_refresh_exposure_ceiling();
	if (g_ae.exposure > g_ae_exp_max) {
		g_ae.exposure = (uint16_t)g_ae_exp_max;
	}
	apply_ae(&g_ae);
	g_ae.settle = TR_AE_SETTLE_FRAMES;
}

/* fix round 12 finding A registers, direct SCCB (same protocol tr_ae_read_
 * vts() above already uses -- 16-bit big-endian register address, 8-bit
 * data). 0x3503's own comment is below, at its one write site. */
#define OV9281_REG_EXP_H   0x3500u
#define OV9281_REG_EXP_M   0x3501u
#define OV9281_REG_EXP_L   0x3502u
#define OV9281_REG_MODE    0x3503u
#define OV9281_REG_GAIN    0x3509u
#define OV9281_MODE_MANUAL 0x08u /* chips/ov9281's own mode-init table value for this register */

/* fix round 14 (silicon regression on 6fbf7b7): round 13 wrapped the writes
 * below in SC_GROUP (0x3208) start/end/launch on the strength of "the
 * standard OmniVision group-hold register" -- unverified against anything
 * this repo can actually check: chips/ov9281's own vendor driver (alp-sdk-
 * lcd's ov9281.c) NEVER TOUCHES 0x3208, and no datasheet is on hand to
 * confirm the register even means group-hold on THIS part rather than
 * something else entirely. On real silicon the very first apply_ae() call
 * (frame ~30) froze the camera stream stone dead -- exactly
 * consistent with 0x3208 being the wrong register: either it silently
 * resets/standbys the sensor, or (the round-13 control-flow bug on top of
 * the same bad guess) the stop-on-first-error chain could leave a genuine
 * group hold STARTED but never ENDed if any write in between failed,
 * wedging the sensor in "buffering, nothing live" forever. Dropped
 * entirely -- back to the plain per-register writes that streamed fine
 * through 2c8969e, keeping round 13's two PROVEN fixes: whole-line
 * exposure units (tr_ae_exposure_regs()'s own comment) and stop-on-first-
 * error (still correct with no group to leave open). A one-frame exposure/
 * gain mismatch at a VSYNC boundary is a cosmetic risk this file accepts
 * rather than re-guess an unverified protocol a second time. */

static uint8_t ov9281_gain_reg(uint8_t idx)
{
	return tr_ae_gain_reg_map[idx <= TR_AE_GAIN_IDX_MAX ? idx : TR_AE_GAIN_IDX_MAX];
}

static int ov9281_write_reg8(uint16_t reg, uint8_t val)
{
	uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)reg, val };

	return i2c_write_dt(&g_sensor_i2c, buf, sizeof(buf));
}

static int ov9281_read_reg8(uint16_t reg, uint8_t *val)
{
	uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };

	return i2c_write_read_dt(&g_sensor_i2c, addr, sizeof(addr), val, 1);
}

/* fix round 12 finding A: chips/ov9281's own ov9281_set_ctrl() clamps
 * VIDEO_CID_EXPOSURE to ctrls->exposure.range.max before writing the
 * register -- always correct in itself (whole lines, fix round 13's own
 * correction), but never re-derived per live VTS the way this app's own
 * ceiling now is, so a raw register write bypasses it regardless: the SAME
 * "SDK gap, direct hardware reach" pattern this file already uses for the
 * VTS read (g_sensor_i2c's own comment). 0x3503 (AEC manual mode) is
 * reasserted every call -- the driver's own mode-init table sets it once
 * (0x08) and never revisits it, so this is a no-op if nothing reset it and
 * a real fix if something did. Gain is a raw write too now (fix round 13:
 * was video_set_ctrl(), moved so it can sit inside the SAME group hold as
 * exposure below -- an OmniVision sensor applying a new exposure one frame
 * and a new gain the next, split across a VSYNC, briefly shows a wrong-
 * brightness frame the group hold exists to prevent -- this file's own
 * header comment above the (now removed) group-hold register block).
 *
 * fix round 14: no group hold (see that comment) -- plain per-register
 * writes, stop on the FIRST failed one (round 12's version pressed on
 * through every write and read regardless -- 4 writes + 5 reads + 1 VTS
 * read = ~10 I2C transactions -- so a genuinely dead sensor stalled this
 * whole call for close to 10x one transaction's own timeout, about 1.1 s,
 * once per AE write). Readback still runs even on a failed
 * write path (ae_write_rc records which; the beacon should show this
 * app's LAST KNOWN state, not stale data from before the failure), but the
 * writes themselves stop at the first error rather than digging the hole
 * deeper against a bus that has already stopped answering. */
static void apply_ae(const tr_ae_t *ae)
{
	uint8_t h, m, l;

	tr_ae_exposure_regs(ae->exposure, &h, &m, &l);

	int rc;

	rc = ov9281_write_reg8(OV9281_REG_MODE, OV9281_MODE_MANUAL);
	if (rc >= 0) {
		rc = ov9281_write_reg8(OV9281_REG_EXP_H, h);
	}
	if (rc >= 0) {
		rc = ov9281_write_reg8(OV9281_REG_EXP_M, m);
	}
	if (rc >= 0) {
		rc = ov9281_write_reg8(OV9281_REG_EXP_L, l);
	}
	if (rc >= 0) {
		rc = ov9281_write_reg8(OV9281_REG_GAIN, ov9281_gain_reg(ae->gain_idx));
	}

	int      rc_worst = rc < 0 ? rc : 0;
	uint8_t  rb_h = 0, rb_m = 0, rb_l = 0, rb_mode = 0, rb_gain = 0;
	uint16_t vts = 0;

	rc       = ov9281_read_reg8(OV9281_REG_EXP_H, &rb_h);
	rc_worst = rc < rc_worst ? rc : rc_worst;
	rc       = ov9281_read_reg8(OV9281_REG_EXP_M, &rb_m);
	rc_worst = rc < rc_worst ? rc : rc_worst;
	rc       = ov9281_read_reg8(OV9281_REG_EXP_L, &rb_l);
	rc_worst = rc < rc_worst ? rc : rc_worst;
	rc       = ov9281_read_reg8(OV9281_REG_MODE, &rb_mode);
	rc_worst = rc < rc_worst ? rc : rc_worst;
	rc       = ov9281_read_reg8(OV9281_REG_GAIN, &rb_gain);
	rc_worst = rc < rc_worst ? rc : rc_worst;
	rc       = tr_ae_read_vts(&vts);
	rc_worst = rc < rc_worst ? rc : rc_worst;

	tr_hp_dbg_write_seq_odd(g_dbg, pslot_barrier);
	g_dbg->ae_reg_exp_h = rb_h;
	g_dbg->ae_reg_exp_m = rb_m;
	g_dbg->ae_reg_exp_l = rb_l;
	g_dbg->ae_reg_mode  = rb_mode;
	g_dbg->ae_reg_gain  = rb_gain;
	g_dbg->ae_reg_vts   = vts;
	g_dbg->ae_write_rc  = rc_worst;
	tr_hp_dbg_write_seq_even(g_dbg, pslot_barrier);
}

/*
 * ---- SRAM1 power gate (fix round 2; round 1's version only reported, and
 * its own check READ SRAM1 -- exactly the memory it existed to protect
 * against touching unconfirmed). CAM_POOL is SRAM1 0x02480000
 * (hp_vision/boards/<board>.overlay's sram1_cam_pool node); SRAM1
 * bus-faults cold on this board (AEN memory note) until the A32/stub boot
 * sequence powers it.
 *
 * This app does NOT need to gate Zephyr's own CONFIG_VIDEO_BUFFER_POOL_
 * ZEPHYR_REGION heap init: it is a LAZY init (zephyr/drivers/video/
 * video_common.c video_buffer_pool_initialized, k_heap_init() on the FIRST
 * video_buffer_alloc() call), not a boot-time SYS_INIT -- round 1's header
 * comment claiming otherwise was wrong, unchecked against the driver
 * source. The first real touch of CAM_POOL is therefore exactly
 * tr_camera_open() below, the ordinary function call this file already
 * controls -- gating THAT is gating the pool's own first use, no
 * dt-deferred-init or custom allocator needed.
 *
 * This board's SoC DFP has no readable "SRAM1 is powered" status bit to
 * gate on (checked: VBAT.RET_CTRL, Device/soc/AE822FA0E5597/include/
 * rtss_hp/soc.h, AIPM's MB_SRAM1_RET / SRAM1_RET_MASK bit 26 in
 * se_services/include/aipm.h -- a deep-sleep retention FORCE/MASK control,
 * not bus-access telemetry; reinterpreting a control bit as a status bit
 * without a bench measurement to check it against is a worse bet than the
 * fallback below). Gate instead on an HE-written ready word OUTSIDE SRAM1:
 * src/platform/a32.c's tr_a32_boot() already has the real confirmation
 * (sram1_answers(), the same fact tr_a32_link_ok() reports) and writes it
 * to TR_MEM_SRAM1_READY, SRAM0 (always-on on this board -- CAM_POOL itself,
 * the pslot, and this file's own hp_dbg_t all already rely on that same
 * fact). This function reads ONLY that SRAM0 word, never SRAM1.
 */
static bool tr_sram1_ready(void)
{
	return sys_read32(TR_MEM_SRAM1_READY) == TR_MEM_SRAM1_READY_MAGIC;
}

/* ---- HUD thumbnail: 10x nearest-sample decimation of the RAW 640x400
 * frame (not the letterboxed/padded 192x192 tensor -- see camera_ae.h's
 * header note on why the letterboxed buffer is a biased source; the same
 * padding-band argument applies here: a 64x40 thumb of the padded tensor
 * would show ~37 % dead grey bands). 640/64 = 400/40 = 10 exactly. */
static uint8_t g_thumb[TR_PSLOT_THUMB_BYTES];

static void make_thumb(const uint8_t *frame, int16_t fw, int16_t fh)
{
	for (int y = 0; y < TR_PSLOT_THUMB_H; y++) {
		int sy = y * fh / TR_PSLOT_THUMB_H;

		for (int x = 0; x < TR_PSLOT_THUMB_W; x++) {
			int sx                            = x * fw / TR_PSLOT_THUMB_W;
			g_thumb[y * TR_PSLOT_THUMB_W + x] = frame[sy * fw + sx];
		}
	}
}

/* The keypoint smoother's state and the time of its last frame (0: none
 * yet -- the first dt is huge, which re-seeds, kp_smooth.h). */
static tr_kp_smooth_t g_smooth;
static int64_t        g_smooth_ms;

/* Stage timing on the SysTick-backed 64-bit kernel cycle counter at the
 * full CPU clock, NOT the DWT CYCCNT (Zephyr's timing_counter_get()): the
 * DWT only counts while DEMCR.TRCENA and DWT_CTRL.CYCCNTENA stay set and
 * debug counting is allowed, all of which a debugger session (or the SE's
 * debug authentication) can change under a running core. Silicon, build
 * 4612458 on 2026W36-0009: CYCCNT stopped ~26 s into the loop -- every *_us 0,
 * busy/total frozen, the HUD "M55-HP 0%" -- while the heartbeat (SysTick)
 * ran on. The kernel clock cannot stop without the kernel stopping. */
typedef uint64_t tr_cyc_t;

BUILD_ASSERT(IS_ENABLED(CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER) &&
                 CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC >= 100000000,
             "stage timing needs a 64-bit kernel cycle counter at the CPU clock (SysTick): "
             "the busy/total cycles and the *_us stages would lose their resolution");

#define TR_CPU_MHZ ((uint32_t)(CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000))

static tr_cyc_t now_cyc(void)
{
	return k_cycle_get_64();
}

static uint32_t us(tr_cyc_t a, tr_cyc_t b)
{
	return (uint32_t)((b - a) / TR_CPU_MHZ);
}

static uint64_t cyc(tr_cyc_t a, tr_cyc_t b)
{
	return b - a;
}

int main(void)
{
	memset((void *)g_dbg, 0, sizeof(*g_dbg));
	g_dbg->magic   = TR_HP_DBG_MAGIC;
	g_dbg->cpuid   = SCB->CPUID;
	g_dbg->vtor    = SCB->VTOR;
	g_dbg->cpu_mhz = TR_CPU_MHZ;

	printk("\n=== trace-runner HP vision (%u MHz) ===\n", g_dbg->cpu_mhz);

	/* The unstick itself already ran (SYS_INIT PRE_KERNEL_1, above) before
	 * any driver init, including the ov9281 sensor's own chip-ID read --
	 * that read is exactly how a still-stuck bus (the unstick sequence not
	 * sufficient this boot) becomes visible: device_is_ready() reports
	 * false because the driver's own init failed on it. */
	bool i2c1_ok = device_is_ready(g_sensor);

	if (!i2c1_ok) {
		printk("RESULT FAIL: ov9281 device not ready -- I2C1 still stuck after unstick\n");
	}

	volatile tr_pslot_t *const    pslot    = (volatile tr_pslot_t *)TR_MEM_PSLOT;
	volatile tr_cam_view_t *const cam_view = (volatile tr_cam_view_t *)TR_MEM_CAM_VIEW;

	memset((void *)pslot, 0, sizeof(*pslot));
	memset((void *)cam_view, 0, sizeof(*cam_view));

	alp_inference_config_t cfg = {
		.model_data  = (const void *)TR_MOVENET_MRAM_ADDR,
		.model_size  = TR_MOVENET_MRAM_SIZE,
		.format      = ALP_INFERENCE_MODEL_VELA,
		.backend     = ALP_INFERENCE_BACKEND_AUTO,
		.arena_bytes = TR_MEM_NPU_ARENA_SIZE,
		.arena       = (void *)TR_MEM_NPU_ARENA,
	};
	alp_inference_t *inf = alp_inference_open(&cfg);

	if (inf == NULL) {
		printk("RESULT FAIL: alp_inference_open -- %s\n", alp_status_name(alp_last_error()));
		g_dbg->status = alp_last_error();
		/* Still publish RUNNING=false forever, so the HE's staleness check
		 * (design sec 3) demotes to its fallback instead of hanging on a
		 * pose that will never come -- there is nothing else useful this
		 * core can do. */
		for (;;) {
			tr_pslot_write(
			    pslot, &(tr_pose_t){ 0 }, 0u, 0u, TR_HP_STATE_NO_CAMERA, NULL, 0u, pslot_barrier);
			g_dbg->heartbeat++;
			k_msleep(200);
		}
	}

	/* Start mid-range; tr_ae_camera_opened() clamps it to the live ceiling. */
	tr_ae_init(&g_ae, (uint16_t)(g_ae_exp_max / 2u), TR_AE_GAIN_IDX_0);

	bool    sram1_ok        = tr_sram1_ready();
	int64_t sram1_t0_ms     = k_uptime_get();
	bool    sram1_fail_seen = false;

	if (sram1_ok) {
		g_dbg->sram1_ready_seen         = 1u;
		g_dbg->sram1_ready_at_heartbeat = g_dbg->heartbeat;
		printk("sram1   : ready word seen -- CAM_POOL (SRAM1) safe to touch\n");
	} else {
		/* Not a failure: the A32 stub may still be powering SRAM1 at HP boot; the retry below
		 * prints "ready word seen (after retry)" when it lands. */
		printk("WARN: SRAM1 ready word not seen yet -- CAM_POOL (SRAM1) NOT touched this "
		       "pass, will retry\n");
	}

	/* Gates the pool's own first real use: tr_camera_open() is where the
	 * lazy CONFIG_VIDEO_BUFFER_POOL_ZEPHYR_REGION heap actually gets
	 * initialised (see tr_sram1_ready()'s header note) and where the CPI
	 * DMA starts writing real frames into it -- so this is a REAL gate,
	 * not a report: SRAM1 is never touched while sram1_ok is false. */
	bool     cam_ok   = i2c1_ok && sram1_ok && (tr_camera_open() == 0);
	uint32_t frame_no = 0;

	if (cam_ok) {
		tr_ae_camera_opened();
	}
	int64_t  hz_t0 = k_uptime_get();
	uint32_t hz_n  = 0;

	/* fix round 14 (silicon regression): the NO_FRAME path below already
	 * published hp_state every empty pass, but never noticed a genuinely
	 * WEDGED stream (round 13's group-hold bug froze the sensor after
	 * frame ~30 and this loop just spun publishing NO_FRAME forever,
	 * heartbeat racing, frame_no never advancing again). last_frame_ms
	 * tracks the last time a real frame came back; >500 ms with none is
	 * treated as a stuck stream, not an ordinary between-frames gap
	 * (30 fps is ~33 ms/frame -- 500 ms is >10 missed frames), and the
	 * stream is torn down and reopened rather than left spinning. */
	int64_t last_frame_ms = k_uptime_get();

	/* HP busy/total cycle counters (fix round 5): total_cyc accumulates the
	 * WHOLE loop period between consecutive iterations -- every path
	 * (camera retry, no-frame, an inference error, the full pipeline)
	 * counts, so a single instrumentation point here covers every `continue`
	 * below without touching each one; busy_cyc (added only on the full
	 * pipeline path, alongside the other g_dbg->*_us fields) is real work
	 * only -- capture-wait is I/O, not load. The HE computes a load % from
	 * the delta between two samples, the same way it already does for
	 * itself (src/hud/hud.c tr_perf_sample()) -- see tr_hp_dbg.h. */
	tr_cyc_t loop_prev       = now_cyc();
	bool     loop_prev_valid = false;

	for (;;) {
		tr_cyc_t loop_now = now_cyc();

		if (loop_prev_valid) {
			g_dbg->total_cyc += cyc(loop_prev, loop_now);
		}
		loop_prev       = loop_now;
		loop_prev_valid = true;

		g_dbg->heartbeat++;

		if (!cam_ok) {
			uint32_t st = !i2c1_ok    ? TR_HP_STATE_I2C_STUCK
			              : !sram1_ok ? TR_HP_STATE_SRAM1_NOT_READY
			                          : TR_HP_STATE_NO_CAMERA;

			tr_pslot_write(pslot, &(tr_pose_t){ 0 }, 0u, 0u, st, NULL, frame_no, pslot_barrier);
			k_msleep(200);
			/* Re-check readiness too: nothing re-runs the unstick after boot
			 * (it is a one-shot SYS_INIT), but the driver itself could still
			 * flip ready on a later retry in some failure modes -- cheap to
			 * ask again, and it costs nothing if the answer never changes.
			 * SRAM1 IS expected to flip true later in a cold-boot race
			 * against the A32/stub, so this is the real recovery path for
			 * it, not just belt-and-braces like the other two. */
			if (!sram1_ok) {
				sram1_ok = tr_sram1_ready();
				if (sram1_ok) {
					g_dbg->sram1_ready_seen         = 1u;
					g_dbg->sram1_ready_at_heartbeat = g_dbg->heartbeat;
					printk("sram1   : ready word seen (after retry) -- CAM_POOL (SRAM1) safe to "
					       "touch\n");
				} else if (!sram1_fail_seen && k_uptime_get() - sram1_t0_ms > 10000) {
					/* The transient case (the A32 stub still powering SRAM1) recovers in a
					 * retry or two; ~10 s of 200 ms retries is a real fault, said once. */
					sram1_fail_seen = true;
					printk("RESULT FAIL: SRAM1 ready word still not seen after ~10 s -- CAM_POOL "
					       "(SRAM1) NOT touched\n");
				}
			}
			i2c1_ok = device_is_ready(g_sensor);
			cam_ok  = i2c1_ok && sram1_ok && (tr_camera_open() == 0);
			if (cam_ok) {
				tr_ae_camera_opened();
			}
			continue;
		}

		tr_cyc_t       tc0 = now_cyc();
		size_t         len;
		const uint8_t *frame = tr_camera_frame(&len);
		tr_cyc_t       tc1   = now_cyc();

		if (frame == NULL) {
			tr_pslot_write(pslot,
			               &(tr_pose_t){ 0 },
			               0u,
			               0u,
			               TR_HP_STATE_NO_FRAME,
			               NULL,
			               frame_no,
			               pslot_barrier);
			g_dbg->capture_us = us(tc0, tc1);
			if (k_uptime_get() - last_frame_ms > 500) {
				/* Stuck, not just between frames -- tear the stream down
				 * and reopen it (tr_camera_open() is a no-op while g_cam
				 * is still set, so close() first is required to actually
				 * re-arm anything). Falls through to the !cam_ok retry
				 * path above on the next iteration if the reopen itself
				 * fails, same recovery already proven for a boot-time
				 * open failure. */
				printk("camera  : no frame for >500 ms -- restarting the stream\n");
				tr_camera_close();
				cam_ok = (tr_camera_open() == 0);
				if (cam_ok) {
					tr_ae_camera_opened();
				}
				last_frame_ms =
				    k_uptime_get(); /* don't re-trip every pass while the reopen settles */
			}
			continue; /* no k_msleep: the caller (design sec 2) drops stale frames, keeps the newest */
		}
		frame_no++;
		last_frame_ms = k_uptime_get();

		/* Live camera PiP (fix round 7 item 5, design sec 13): publish the
		 * buffer's own address, not a copy -- the pixels stay in CAM_POOL
		 * (SRAM1), already proven safe for cross-core reads (this is the
		 * SAME memory the NPU pre-process below reads). Published for
		 * EVERY captured frame, before the NPU pipeline runs, so the PiP
		 * stays live even on a frame the rest of this loop later rejects
		 * (a bad inference tensor, no NPU output, etc). tr_camera_release()
		 * below is UNCHANGED (still right after the pre-process's own
		 * read) -- tr_cam_view.h's header note is the accepted tear risk
		 * this ordering trades for zero throughput cost. */
		tr_cam_view_write(cam_view,
		                  (uint32_t)(uintptr_t)frame,
		                  frame_no,
		                  (uint16_t)tr_camera_width(),
		                  (uint16_t)tr_camera_height(),
		                  TR_CAM_ROTATE,
		                  g_cam_mirrored,
		                  pslot_barrier);

		tr_cyc_t ta0 = now_cyc();

		/* Every frame: the controller itself skips the frame after a write
		 * (camera_ae.h), metering is a 1/16 subsample. */
		if (tr_ae_step(&g_ae,
		               tr_ae_meter(frame, tr_camera_width(), tr_camera_height()),
		               TR_AE_EXP_MIN,
		               (uint16_t)g_ae_exp_max,
		               TR_AE_GAIN_IDX_MAX)) {
			apply_ae(&g_ae);
		}
		tr_cyc_t ta1 = now_cyc();

		/* Live AE state, for the bench/HUD (fix round 5) -- mirrored every
		 * frame. */
		g_dbg->ae_exposure = g_ae.exposure;
		g_dbg->ae_gain_idx = g_ae.gain_idx;

		alp_inference_tensor_t in = { 0 };

		if (alp_inference_get_input(inf, 0, &in) != ALP_OK ||
		    in.size_bytes != TR_MN_IN * TR_MN_IN * 3) {
			g_dbg->status = alp_last_error();
			tr_camera_release();
			continue;
		}

		tr_cyc_t t0 = now_cyc();

		/* The sensor is mounted on its side (src/vision/cam_rot.h): the
		 * model sees the UPRIGHT image, rotated inside this same letterbox
		 * pass (no rotated copy of the frame). */
		tr_movenet_input_rot(frame, tr_camera_width(), tr_camera_height(), TR_CAM_ROTATE, in.data);

		tr_cyc_t     t1 = now_cyc();
		alp_status_t st = alp_inference_invoke(inf);
		tr_cyc_t     t2 = now_cyc();

		if (st != ALP_OK) {
			g_dbg->status = st;
			tr_camera_release();
			tr_pslot_write(pslot,
			               &(tr_pose_t){ 0 },
			               0u,
			               0u,
			               TR_HP_STATE_NO_CAMERA,
			               NULL,
			               frame_no,
			               pslot_barrier);
			continue;
		}

		alp_inference_tensor_t o[4];
		bool                   out_ok = true;

		for (int i = 0; i < 4; i++) {
			if (alp_inference_get_output(inf, i, &o[i]) != ALP_OK) {
				out_ok = false;
				break;
			}
		}
		if (!out_ok) {
			g_dbg->status = alp_last_error();
			tr_camera_release();
			continue;
		}

		/* tr_movenet_decode() copies the centre and heat maps out of the arena (movenet.c): a
		 * model with smaller output tensors would be read past its end. */
		if (o[0].size_bytes < TR_MN_CELLS || o[1].size_bytes < TR_MN_CELLS * TR_POSE_KP) {
			g_dbg->status = ALP_ERR_INVAL;
			tr_camera_release();
			continue;
		}

		tr_movenet_out_t mo = { o[0].data, o[1].data, o[2].data, o[3].data };
		tr_pose_t        pose;

		/* Keypoints in UPRIGHT frame px (portrait when rotated): what the
		 * A32 skeleton and the HE's tracker both read. */
		tr_movenet_decode(&mo,
		                  TR_CAM_ROTATE != 0 ? tr_camera_height() : tr_camera_width(),
		                  TR_CAM_ROTATE != 0 ? tr_camera_width() : tr_camera_height(),
		                  &pose);

		/* Temporal smoothing before the pose is published (src/vision/
		 * kp_smooth.h, One-Euro per keypoint): Lightning's frame-to-frame
		 * jitter damped, a jump/duck/lane step still through within 2
		 * frames. dt is this core's own frame clock; the first frame and a
		 * stalled stream (dt > TR_KS_DT_MAX) re-seed. Counted in decode_us. */
		int64_t pose_ms = k_uptime_get();

		tr_kp_smooth_step(&g_smooth, &pose, (uint32_t)(pose_ms - g_smooth_ms));
		g_smooth_ms = pose_ms;

		tr_cyc_t t3 = now_cyc();

		make_thumb(frame, tr_camera_width(), tr_camera_height());

		tr_cyc_t t4 = now_cyc();

		tr_camera_release();

		/* fix round 11: this whole group (the *_us timing block) is now
		 * seqlock-protected -- a reader landing mid-update (six separate
		 * stores) used to see an inconsistent snapshot (the exact silicon
		 * finding: "one beacon read per boot was torn, all timings 0"),
		 * the same class of gap tr_pslot_t already closed for its own
		 * body. busy_cyc keeps its own tr_hp_dbg_read_stable() protection
		 * too (unchanged, hud_l2.c) -- being also inside this window is
		 * harmless, not a replacement for it. */
		tr_hp_dbg_write_seq_odd(g_dbg, pslot_barrier);
		g_dbg->frame_no   = frame_no;
		g_dbg->capture_us = us(tc0, tc1);
		g_dbg->ae_us      = us(ta0, ta1);
		g_dbg->pre_us     = us(t0, t1);
		g_dbg->infer_us   = us(t1, t2);
		g_dbg->decode_us  = us(t2, t3);
		g_dbg->thumb_us   = us(t3, t4);
		/* Real work only (fix round 5): AE + letterbox + invoke + decode +
		 * thumbnail -- NOT capture_us (I/O wait, not load). */
		g_dbg->busy_cyc += cyc(ta0, ta1) + cyc(t0, t1) + cyc(t1, t2) + cyc(t2, t3) + cyc(t3, t4);
		tr_hp_dbg_write_seq_even(g_dbg, pslot_barrier);

		tr_pslot_write(pslot,
		               &pose,
		               g_dbg->infer_us,
		               g_dbg->pre_us,
		               TR_HP_STATE_RUNNING,
		               g_thumb,
		               frame_no,
		               pslot_barrier);

		hz_n++;
		int64_t now = k_uptime_get();

		if (now - hz_t0 >= 1000) {
			tr_hp_dbg_write_seq_odd(g_dbg, pslot_barrier);
			g_dbg->loop_hz_x10 = (uint32_t)(hz_n * 10000 / (now - hz_t0));
			tr_hp_dbg_write_seq_even(g_dbg, pslot_barrier);
			hz_t0 = now;
			hz_n  = 0;
		}
	}
	return 0;
}
