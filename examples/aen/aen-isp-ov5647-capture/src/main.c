/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-isp-ov5647-capture -- the Alif ISP-Pico (VeriSilicon ISP Nano,
 * compatible "vsi,isp-pico") bring-up on the E1M-AEN801/AEN803 (Ensemble
 * E8, M55-HE): a REAL OV5647 sensor frame through the ISP
 * (sensor -> csi -> cam -> isp -> memory), with AE (auto exposure/gain) and
 * AWB (auto white balance) running through isp_pico.c's standard Zephyr
 * video ctrl registry (VIDEO_CID_EXPOSURE_AUTO / VIDEO_CID_AUTO_WHITE_BALANCE
 * on the ISP device) instead of a fixed manual exposure -- the whole point
 * of driving a real ISP rather than a raw sensor capture.
 *
 * RECIPE: mirrors Alif's own sdk-alif GitHub samples/drivers/viewfinder
 * (built for an OV5675, a different OmniVision MIPI sensor, through this
 * same ISP): the boards/ overlays wire sensor -> csi port@0 ... csi port@2 ->
 * cam port@0; cam port@2 -> isp port (the ISP is the CPI's ONLY consumer --
 * no cam port@1/AXI-memory path). ISP INPUT format is VIDEO_PIX_FMT_SBGGR10P
 * at the sensor's native 640x480 -- our OV5647 is a BGGR sensor and
 * advertises SBGGR10P natively (ov5647.c); the hal_alif wrapper maps it to
 * PIXEL_FORMAT_BGGR10 (this branch's hal_alif patch 0003) -- the CORRECT
 * Bayer phase, unlike the VIDEO_PIX_FMT_Y10P this example used to request
 * (which the wrapper only maps to PIXEL_FORMAT_GRBG10, wrong for a BGGR
 * sensor -- swapped colour channels). ISP OUTPUT is VIDEO_PIX_FMT_YUV420
 * 640x480, pitch = width*3/2.
 *
 * AE + AWB: both are controlled through the SAME chain every other video
 * ctrl on this device uses (isp -> cam -> csi -> sensor, each declaring a
 * VIDEO_DEVICE_DEFINE -- isp_pico.c, video_alif.c, video_csi_dw.c,
 * ov5647.c). AWB defaults ON at the driver level (isp_pico.c's
 * isp_init_controls(), matching the calibration's own OP_TYPE_AUTO AWB --
 * hal_alif patch 0009); this app's explicit AUTO_WHITE_BALANCE=1 below is
 * now a no-op that just documents the intent. AE ALSO defaults AUTO --
 * isp_pico.c's isp_apply_ae_sensor_gate(), called unconditionally from
 * isp_stream_start() in BOTH lib AE modes (not something this app's ctrl
 * call turns on), forces the sensor's OWN EXPOSURE_AUTO/AUTOGAIN to manual
 * so the ISP's write-back (VIDEO_CID_EXPOSURE/VIDEO_CID_ANALOGUE_GAIN via
 * hal_alif patch 0005) doesn't fight the sensor's own AEC. Separately,
 * isp_apply_ae() derives the library's own exposure ceiling from the
 * sensor's ACTUAL configured frame rate (video_get_frmival(), not an
 * assumed one) -- see isp_pico.c's isp_apply_ae() for the full derivation.
 *
 * FRAME RATE: OV5647's achievable rates are ov5647.c's ov5647_framerates[]
 * = {10, 15, 30, 45, 60, 90, 120}; 10 fps (the floor) gives AE the most
 * exposure headroom for a dim scene without going below what the sensor
 * driver can actually deliver (video_set_frmival() clamps/rounds to the
 * nearest supported entry regardless of what's requested -- this app reads
 * back the EFFECTIVE rate via video_get_frmival() and prints it rather than
 * assuming the request was honoured verbatim).
 *
 * The captured frame is copied into a separate static SRAM0 buffer
 * (frame_copy, below) right after the last dequeue, before the bench hold.
 *
 * CALIBRATION STATUS: hal_alif patch 0008 + CONFIG_VIDEO_ISP_VSI_CALIB_OV5647
 * (default y for this board) ship an OV5647 AWB table fitted to this
 * INNO-MAKER module -- scaled from Raspberry Pi libcamera's ov5647.json
 * reference onto the untouched ARX3A0 table's own internal gain relation,
 * then bench-checked on ONE scene (runs 159/162). Provisional: a grey-card
 * calibration under two illuminants is the intended replacement. CCM is ON
 * (CONFIG_ISP_LIB_CCM_MODULE=y in this example's prj.conf) -- runs
 * 171/172 (colour-bar test pattern, all 8 bars at correct hue and full
 * saturation, CCM on -- run 172 through <alp/camera.h>) and run 178
 * (faded and dark bars land exactly where expected, e.g. pure red at raw
 * 110 gives V +50 = BT.601) show the ISP pipeline, CCM included, is
 * colour-correct. Run 176 (a same-frame CPI raw-vs-ISP comparison) shows
 * the ISP matches the raw scene to 1.5%, and the raw scene itself varies
 * only ~4% (white ceiling raw R/G 0.98, B/G 0.97) -- so the muted colour
 * and the residual cast on the ceiling (runs 163/164) come from the
 * sensor/lens (likely an IR leak) plus this provisional calibration, not
 * from the ISP: CCM amplifies an existing low-saturation cast rather than
 * causing it, and will be retuned by that grey-card calibration rather
 * than by turning CCM off.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/crc.h>
#include <zephyr/drivers/video.h>
#include <zephyr/drivers/video-controls.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/video/isp-vsi.h>

#define ISP_NODE    DT_NODELABEL(isp)
#define OV5647_NODE DT_NODELABEL(ov5647)

/*
 * isp_vsi_register_ae_status_callback() (isp_pico.c, exported via the
 * vendored isp-vsi.h) is a genuinely public, reachable driver API -- not an
 * escape hatch -- that wires isp_vsi_bottom_half()'s own
 * VSI_MPI_ISP_QueryExposureInfo() (isp_api_wrapper.c) isStable flag out to
 * app code. It fires from isp_bottom_half()'s workqueue context, so the
 * callback just latches the value; main() prints it periodically instead
 * of reading it inline.
 */
static volatile uint8_t g_ae_stable;

static void ae_status_cb(const struct device *dev, uint8_t ae_stable, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	g_ae_stable = ae_stable;
}

/* CONFIG_VIDEO_ISP_VSI_FRAME_STATS=y (prj.conf): counts MI_INTR_MP_FRAME_END
 * events, i.e. the ISP's own "frame complete" IRQ actually firing --
 * isp_pico.c. */
extern volatile uint32_t isp_mi_frame_end_count;

#define FRAME_WIDTH    640
#define FRAME_HEIGHT   480
#define FRAME_SIZE     (FRAME_WIDTH * FRAME_HEIGHT * 3 / 2) /* YUV420 planar */
#define N_BUFFERS      2
#define N_FRAMES       30
#define REG_DUMP_EVERY 5

static uint8_t frame_copy[FRAME_SIZE] __attribute__((section("SRAM0"), aligned(64)));

#define ISP_BASE          (0x49046000UL)
#define ISP_DGAIN_RB      (ISP_BASE + 0x800) /* isp_pico.h:95 */
#define ISP_DGAIN_G       (ISP_BASE + 0x804) /* isp_pico.h:96 */
#define ISP_AWB_WHITE_CNT (ISP_BASE + 0x980) /* isp_pico.h:146 */
#define ISP_AWB_MEAN      (ISP_BASE + 0x984) /* isp_pico.h:147 */
/* Run 84/85: EXPM (AE measurement) grid hardware registers -- not yet in
 * isp_pico.h (no driver code reads them, this bench dump is the first
 * consumer). Run 84 passed isp_apply_aem_wbm() (isp_pico.c) width/5,
 * height/5 (128/96) and this readback showed 24/18 -- the library divides
 * the window by its OWN internal 5x5 grid, so 128/96 in became 24/18
 * (128/96 divided by 5 AGAIN). Run 85: isp_apply_aem_wbm() now passes the
 * FULL frame (640/480); expect THIS readback to land on 128/96 instead.
 * ISP_EXP_MEAN_22 is the EXPM grid's block index 22 (of the 25-block, 5x5
 * grid) mean luminance -- nonzero is proof the grid completed a real
 * measurement (run 84's bug: 0 forever).
 */
#define ISP_EXP_H_SIZE  (ISP_BASE + 0x72C)
#define ISP_EXP_V_SIZE  (ISP_BASE + 0x730)
#define ISP_EXP_MEAN_22 (ISP_BASE + 0x764)
/* Run 85: WBM (AWB measurement) grid hardware registers -- same "does the
 * window geometry the driver programmed match what actually landed"
 * question as the EXPM registers above, for the white-balance measurement
 * side. Expect ISP_AWB_WHITE_CNT (already dumped below) around 0x0004B000
 * in this dark bench scene once isp_apply_aem_wbm()'s 0xF0 RGB-mode upper
 * bounds (run 85, isp_pico.c) admit real (non-clipped) pixels instead of
 * excluding everything (run 84/85's WBM count 0 bug).
 */
#define ISP_AWB_H_SIZE (ISP_BASE + 0x95C)
#define ISP_AWB_V_SIZE (ISP_BASE + 0x960)
#define ISP_AWB_FRAMES (ISP_BASE + 0x964)

static inline uint32_t reg32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

/* Hand-rolled CCI read: OV5647's own exposure/gain registers, independent
 * of the driver's private video_common.h CCI helper (not on this app's
 * include path) -- same wire protocol, the public zephyr/drivers/i2c.h API. */
static int ov5647_read_reg8(const struct i2c_dt_spec *i2c, uint16_t reg, uint8_t *val)
{
	uint8_t reg_be[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };

	return i2c_write_read_dt(i2c, reg_be, sizeof(reg_be), val, 1);
}

static void print_ov5647_ae_regs(int f)
{
#if DT_NODE_EXISTS(OV5647_NODE)
	const struct i2c_dt_spec i2c = I2C_DT_SPEC_GET(OV5647_NODE);
	/* 0x3500..0x3502 = 20-bit exposure MSB/mid/LSB (1/16-line units),
	 * 0x350a/0x350b = 10-bit AGC gain (register 0x10 = 1x). */
	uint16_t regs[]                 = { 0x3500, 0x3501, 0x3502, 0x350a, 0x350b };
	uint8_t  vals[ARRAY_SIZE(regs)] = { 0 };
	uint32_t exposure_lines16       = 0;
	uint32_t gain_reg;

	for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
		(void)ov5647_read_reg8(&i2c, regs[i], &vals[i]);
	}

	exposure_lines16 =
	    ((uint32_t)vals[0] << 12) | ((uint32_t)vals[1] << 4) | ((uint32_t)vals[2] >> 4);
	gain_reg = ((uint32_t)vals[3] << 8) | vals[4];

	printk("f%d ov5647 0x3500-02=%02x%02x%02x (exposure=%u/16 lines=~%u.%01u) "
	       "0x350a-0b=%02x%02x (gain reg=%u =~%u.%02ux)\n",
	       f,
	       vals[0],
	       vals[1],
	       vals[2],
	       exposure_lines16,
	       exposure_lines16 / 16,
	       (exposure_lines16 % 16) * 10 / 16,
	       vals[3],
	       vals[4],
	       gain_reg,
	       gain_reg / 16,
	       (gain_reg % 16) * 100 / 16);
#else
	printk("f%d OV5647_NODE not in DT; skipping register dump\n", f);
#endif
	/*
	 * ISP_AWB_MEAN layout, per the libisp log string ("noWhitePixel %d
	 * meanCr_R %d meanY_G %d meanCb_B %d"): R = bits[7:0], B = bits[15:8],
	 * G = bits[23:16].
	 */
	uint32_t awb_mean   = reg32(ISP_AWB_MEAN);
	uint32_t awb_mean_r = awb_mean & 0xFF;
	uint32_t awb_mean_b = (awb_mean >> 8) & 0xFF;
	uint32_t awb_mean_g = (awb_mean >> 16) & 0xFF;

	printk("f%d DGAIN_RB=0x%08x DGAIN_G=0x%08x AWB_WHITE_CNT=0x%08x "
	       "AWB_MEAN=0x%08x (R=%u G=%u B=%u) ae_stable=%u\n",
	       f,
	       reg32(ISP_DGAIN_RB),
	       reg32(ISP_DGAIN_G),
	       reg32(ISP_AWB_WHITE_CNT),
	       awb_mean,
	       awb_mean_r,
	       awb_mean_g,
	       awb_mean_b,
	       g_ae_stable);

	/* Run 85: h_size/v_size unit verification -- see the #define comment
	 * above. 128/96 now expected (run 84 read back 24/18 with the old
	 * width/5,height/5 driver code).
	 */
	printk("f%d ISP_EXP_H_SIZE=%u ISP_EXP_V_SIZE=%u ISP_EXP_MEAN_22=%u\n",
	       f,
	       reg32(ISP_EXP_H_SIZE),
	       reg32(ISP_EXP_V_SIZE),
	       reg32(ISP_EXP_MEAN_22));
	printk("f%d ISP_AWB_H_SIZE=%u ISP_AWB_V_SIZE=%u ISP_AWB_FRAMES=%u\n",
	       f,
	       reg32(ISP_AWB_H_SIZE),
	       reg32(ISP_AWB_V_SIZE),
	       reg32(ISP_AWB_FRAMES));
}

struct plane_stats {
	uint32_t mean;
	uint32_t min;
	uint32_t max;
};

static void plane_stats_compute(const uint8_t *plane, size_t n, struct plane_stats *st)
{
	uint32_t sum = 0;
	uint8_t  mn = 255, mx = 0;

	for (size_t i = 0; i < n; i++) {
		uint8_t v = plane[i];

		sum += v;
		mn = MIN(mn, v);
		mx = MAX(mx, v);
	}

	st->mean = n ? sum / n : 0;
	st->min  = mn;
	st->max  = mx;
}

int main(void)
{
	printk("\n=== aen-isp-ov5647-capture (real OV5647 through the ISP, AE+AWB on) ===\n");

	const struct device *isp_dev = DEVICE_DT_GET_OR_NULL(ISP_NODE);

	if (isp_dev == NULL || !device_is_ready(isp_dev)) {
		printk("RESULT FAIL: isp device %s\n", isp_dev == NULL ? "not built" : "not ready");
		return -1;
	}

	int rc_ae_cb = isp_vsi_register_ae_status_callback(isp_dev, ae_status_cb, NULL);

	printk("isp_vsi_register_ae_status_callback rc=%d\n", rc_ae_cb);

#if DT_NODE_EXISTS(OV5647_NODE)
	const struct device *sensor_dev = DEVICE_DT_GET(OV5647_NODE);

	if (device_is_ready(sensor_dev)) {
		/* OV5647's floor rate (ov5647_framerates[]) -- see the file
		 * header for why 10 fps, not a lower "request" that would
		 * just get silently clamped to it anyway. */
		struct video_frmival frmival    = { .numerator = 1, .denominator = 10 };
		int                  rc_frmival = video_set_frmival(sensor_dev, &frmival);

		printk("sensor frmival request 1/10 (10 fps) rc=%d\n", rc_frmival);

		struct video_frmival effective = { 0 };

		if (video_get_frmival(sensor_dev, &effective) == 0 && effective.denominator > 0) {
			printk("sensor frmival EFFECTIVE %u/%u (%u.%02u fps)\n",
			       effective.numerator,
			       effective.denominator,
			       effective.denominator / effective.numerator,
			       (effective.denominator % effective.numerator) * 100 / effective.numerator);
		}

#if !defined(CONFIG_ISP_LIB_AE_MODULE)
		/*
		 * Bench run 81, decisive test image A ("AWB without AE",
		 * overlay-no-ae.conf): with the AE module compiled out,
		 * isp_apply_ae() (isp_pico.c) is a no-op regardless of the
		 * VIDEO_CID_EXPOSURE_AUTO ctrl below -- it never touches the
		 * sensor. Set exposure/gain manually here instead: manual
		 * exposure, 3145 lines (isp_pico.h/sensor_attributes.h's
		 * 10-fps maxIntLine, hal_alif patch 0006) in 1/16-line units,
		 * autogain off, ~16x gain (register 0x100 = 256 decimal,
		 * OV5647_AGC_GAIN 0x350a, register 0x10 = 1x).
		 */
		struct video_control sensor_ctrl;

		sensor_ctrl.id         = VIDEO_CID_EXPOSURE_AUTO;
		sensor_ctrl.val        = VIDEO_EXPOSURE_MANUAL;
		int rc_sensor_exp_auto = video_set_ctrl(sensor_dev, &sensor_ctrl);

		printk("sensor EXPOSURE_AUTO=manual rc=%d\n", rc_sensor_exp_auto);

		sensor_ctrl.id    = VIDEO_CID_EXPOSURE;
		sensor_ctrl.val   = 3145 * 16;
		int rc_sensor_exp = video_set_ctrl(sensor_dev, &sensor_ctrl);

		printk("sensor EXPOSURE=%d rc=%d\n", sensor_ctrl.val, rc_sensor_exp);

		sensor_ctrl.id         = VIDEO_CID_AUTOGAIN;
		sensor_ctrl.val        = 0;
		int rc_sensor_autogain = video_set_ctrl(sensor_dev, &sensor_ctrl);

		printk("sensor AUTOGAIN=0 rc=%d\n", rc_sensor_autogain);

		sensor_ctrl.id     = VIDEO_CID_ANALOGUE_GAIN;
		sensor_ctrl.val    = 0x100;
		int rc_sensor_gain = video_set_ctrl(sensor_dev, &sensor_ctrl);

		printk("sensor ANALOGUE_GAIN=0x%x (~16x) rc=%d\n", sensor_ctrl.val, rc_sensor_gain);
#endif /* !defined(CONFIG_ISP_LIB_AE_MODULE) */
	} else {
		printk("sensor device not ready; leaving frmival at its default\n");
	}
#endif

	/* AWB is already ON by default at the driver level (matches the
	 * calibration's own OP_TYPE_AUTO AWB, hal_alif patch 0009) -- this SET
	 * is explicit intent, not a required enable. It still enables the WB
	 * block (params.wb.enable, isp_apply_wb()) if a future driver default
	 * changes; when CONFIG_VIDEO_ISP_VSI_WB_MANUAL_GAIN is set (image F,
	 * run 84) the driver overrides op_mode to MANUAL and forces its own
	 * fixed gains regardless of this ctrl's val -- WB stays enabled
	 * either way, only auto-vs-manual and the gain source change.
	 */
	struct video_control awb_ctrl = { .id = VIDEO_CID_AUTO_WHITE_BALANCE, .val = 1 };
	int                  rc_awb   = video_set_ctrl(isp_dev, &awb_ctrl);

	printk("video_set_ctrl(ISP, AUTO_WHITE_BALANCE=1) rc=%d (manual_gain_override=%d)\n",
	       rc_awb,
	       IS_ENABLED(CONFIG_VIDEO_ISP_VSI_WB_MANUAL_GAIN));

#if defined(CONFIG_ISP_LIB_AE_MODULE)
	/*
	 * Run 83: guarded on CONFIG_ISP_LIB_AE_MODULE now -- with the module
	 * compiled out (overlay-no-ae.conf's image A), the ISP device never
	 * registers VIDEO_CID_EXPOSURE_AUTO (isp_init_controls(), isp_pico.c),
	 * so calling video_set_ctrl(isp_dev, ...) unconditionally FELL
	 * THROUGH the v4.4 control-registry chain (video_find_ctrl(),
	 * drivers/video/video_ctrls.c) straight to the SENSOR's own
	 * VIDEO_CID_EXPOSURE_AUTO registration (ov5647.c) -- silently
	 * re-enabling the sensor's own AEC and undoing the manual exposure
	 * this file's earlier #if !defined(CONFIG_ISP_LIB_AE_MODULE) branch
	 * had just set (run 83's image A: sensor AEC railed at 502 lines,
	 * black). AE is already AUTO by default at the driver level (matches
	 * the calibration's own OP_TYPE_AUTO AE, hal_alif patch 0009) -- this
	 * SET is explicit intent, not a required enable. Forcing the
	 * sensor's OWN AEC/AUTOGAIN out of the way is isp_pico.c's
	 * isp_apply_ae_sensor_gate(), called unconditionally from
	 * isp_stream_start() in both lib AE modes -- it does not depend on
	 * this SET at all, only on CONFIG_ISP_LIB_AE_MODULE being compiled
	 * in (hence this whole block's #if).
	 */
	struct video_control ae_ctrl = { .id = VIDEO_CID_EXPOSURE_AUTO, .val = VIDEO_EXPOSURE_AUTO };
	int                  rc_ae   = video_set_ctrl(isp_dev, &ae_ctrl);

	printk("video_set_ctrl(ISP, EXPOSURE_AUTO=AUTO) rc=%d\n", rc_ae);
#endif /* defined(CONFIG_ISP_LIB_AE_MODULE) */

	struct video_format in_fmt = {
		.type        = VIDEO_BUF_TYPE_INPUT,
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width       = FRAME_WIDTH,
		.height      = FRAME_HEIGHT,
	};
	int rc_in = video_set_format(isp_dev, &in_fmt);

	printk("video_set_format(INPUT, SBGGR10P, %ux%u) rc=%d\n",
	       (unsigned int)FRAME_WIDTH,
	       (unsigned int)FRAME_HEIGHT,
	       rc_in);

	struct video_format out_fmt = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_YUV420,
		.width       = FRAME_WIDTH,
		.height      = FRAME_HEIGHT,
		.pitch       = FRAME_WIDTH * 3 / 2,
	};
	int rc_out = video_set_format(isp_dev, &out_fmt);

	printk("video_set_format(OUTPUT, YUV420, %ux%u) rc=%d\n",
	       (unsigned int)FRAME_WIDTH,
	       (unsigned int)FRAME_HEIGHT,
	       rc_out);

	if (rc_in || rc_out) {
		printk("RESULT FAIL: format negotiation failed (in=%d out=%d)\n", rc_in, rc_out);
		return -1;
	}

	struct video_buffer *buffers[N_BUFFERS];

	for (int i = 0; i < N_BUFFERS; i++) {
		buffers[i] = video_buffer_aligned_alloc(FRAME_SIZE, 64, K_NO_WAIT);
		if (buffers[i] == NULL) {
			printk("RESULT FAIL: video_buffer_aligned_alloc[%d](%u) returned NULL\n",
			       i,
			       (unsigned int)FRAME_SIZE);
			return -1;
		}
		buffers[i]->type = VIDEO_BUF_TYPE_OUTPUT;
		printk("buffer[%d]: addr=%p size=%u\n", i, (void *)buffers[i]->buffer, buffers[i]->size);

		int rc_enq = video_enqueue(isp_dev, buffers[i]);

		printk("video_enqueue[%d] rc=%d\n", i, rc_enq);
	}

	printk("settling 1000 ms before video_stream_start (Alif's own delay)...\n");
	k_msleep(1000);

	int rc_start = video_stream_start(isp_dev, VIDEO_BUF_TYPE_OUTPUT);

	printk("video_stream_start rc=%d\n", rc_start);

	bool have_last_frame = false;

	for (int f = 1; f <= N_FRAMES; f++) {
		/* isp_pico.c auto-stops once its IN-FIFO empties and only
		 * restarts when this app calls video_stream_start() again
		 * (Alif's own model, no driver-driven restart of its own) --
		 * so this call is the PRIMARY restart path, mirroring Alif's
		 * own reference pattern. -EBUSY (the expected result whenever
		 * the driver hasn't yet auto-stopped between frames) is
		 * handled below either way. */
		int rc_restart = video_stream_start(isp_dev, VIDEO_BUF_TYPE_OUTPUT);

		if (rc_restart && rc_restart != -EBUSY) {
			printk("f%d: stream_start restart rc=%d (FAIL)\n", f, rc_restart);
		}

		struct video_buffer *deq    = NULL;
		int                  rc_deq = video_dequeue(isp_dev, &deq, K_MSEC(2000));

		printk("f%d dequeue rc=%d frame_end_count=%u\n", f, rc_deq, isp_mi_frame_end_count);

		if (rc_deq != 0 || deq == NULL) {
			printk("f%d: RESULT FAIL (no buffer)\n", f);
			break;
		}

#if defined(CONFIG_AEN_ISP_AWB_LIVE_TOGGLE_TEST)
		if (f == 3 || f == 6) {
			struct video_control toggle = {
				.id  = VIDEO_CID_AUTO_WHITE_BALANCE,
				.val = (f == 3) ? 0 : 1,
			};
			int rc_toggle = video_set_ctrl(isp_dev, &toggle);

			printk("f%d video_set_ctrl(ISP, AUTO_WHITE_BALANCE=%d) LIVE rc=%d\n",
			       f,
			       toggle.val,
			       rc_toggle);
		}
#endif /* defined(CONFIG_AEN_ISP_AWB_LIVE_TOGGLE_TEST) */

		const uint8_t     *y_plane = deq->buffer;
		const uint8_t     *u_plane = y_plane + (size_t)FRAME_WIDTH * FRAME_HEIGHT;
		const uint8_t     *v_plane = u_plane + (size_t)(FRAME_WIDTH / 2) * (FRAME_HEIGHT / 2);
		struct plane_stats sy, su, sv;

		plane_stats_compute(y_plane, (size_t)FRAME_WIDTH * FRAME_HEIGHT, &sy);
		plane_stats_compute(u_plane, (size_t)(FRAME_WIDTH / 2) * (FRAME_HEIGHT / 2), &su);
		plane_stats_compute(v_plane, (size_t)(FRAME_WIDTH / 2) * (FRAME_HEIGHT / 2), &sv);

		/* Run 83: bound RAM console volume (CONFIG_RAM_CONSOLE_BUFFER_SIZE
		 * is finite) so the library's own per-frame AwbRun INFO line
		 * (CONFIG_LOG_DEFAULT_LEVEL=3, this diagnostic's overlay) has
		 * room to survive -- every 5th frame (plus frame 1), not every
		 * frame. */
		if (f == 1 || f % REG_DUMP_EVERY == 0) {
			printk("f%d Y mean=%u min=%u max=%u\n", f, sy.mean, sy.min, sy.max);
			printk("f%d U mean=%u min=%u max=%u\n", f, su.mean, su.min, su.max);
			printk("f%d V mean=%u min=%u max=%u\n", f, sv.mean, sv.min, sv.max);
			print_ov5647_ae_regs(f);
		}

		if (f == N_FRAMES) {
			memcpy(frame_copy, deq->buffer, deq->bytesused);
			have_last_frame = true;
		}

		int rc_enq = video_enqueue(isp_dev, deq);

		printk("f%d re-enqueue rc=%d\n", f, rc_enq);
	}

	video_stream_stop(isp_dev, VIDEO_BUF_TYPE_OUTPUT);

	if (have_last_frame) {
		uint32_t crc = crc32_ieee(frame_copy, FRAME_SIZE);

		printk("snapshot(frame %d): addr=%p size=%u crc32=0x%08x\n",
		       N_FRAMES,
		       (void *)frame_copy,
		       (unsigned int)FRAME_SIZE,
		       crc);
		printk("RESULT PASS: %d frame(s) captured (see per-frame stats above)\n", N_FRAMES);
	} else {
		printk("RESULT FAIL: frame %d never captured\n", N_FRAMES);
	}

	printk("Holding 20 s for a bench `savebin` of the snapshot buffer...\n");
	k_sleep(K_SECONDS(20));

	return 0;
}
