/* probe/npu/src/main.c -- M55-HP NPU body-control probe, no camera.
 *
 * Runs the Vela-compiled cut MoveNet (tools/movenet_cut.py) on the HP's
 * Ethos-U through <alp/inference.h>, on canned 640x400 GREY8 frames from the
 * MRAM payload (tools/npu_probe_payload.py, layout src/payload.h), through
 * the SAME pre- and post-processing the game will use (src/vision/movenet.c,
 * pose.c). Times each stage with the DWT cycle counter and checks the decoded
 * pose against the host's expectation. See README.md.
 *
 * Two passes: A with the model copied into SRAM0 (the proven NPU placement,
 * alp-sdk aen-npu-inference-alp-u55), then B with the NPU reading the weights
 * straight from MRAM (the placement the game wants: no 2.4 MB SRAM copy). A is
 * recorded before B starts, so a B that faults still leaves A's numbers.
 */
#include <stdlib.h>
#include <string.h>

#include <cmsis_core.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/timing/timing.h>

#include <alp/inference.h>
#include <alp/peripheral.h>

#include "../../../src/vision/movenet.h"
#include "../../../src/vision/pose.h"
#include "../../../src/ipc/tr_memmap.h"
#include "payload.h"

#define ARENA_BYTES (300u * 1024u) /* Vela: 277.5 KiB SRAM for the cut model on U55-256 */
#define MODEL_MAX   (2560u * 1024u)
#define N_INVOKE    16

/*
 * Results, in SRAM0 at a fixed address so SWD can read them whatever the
 * HP's console state: words 0..3 are the layout scripts/bench/aen/
 * flash-jlink-hp.sh reads as its beacon (magic, CPUID, VTOR, heartbeat), so
 * pass it 0x0237F200. 0x0237F000 page: the sound ring's, never mapped by the
 * A32 (docs/superpowers/specs/2026-09-24-npu-body-control-design.md).
 */
#define RESULT_ADDR  TR_MEM_PSLOT
#define RESULT_MAGIC 0x4E505552u /* 'RUPN' */

typedef struct {
	uint32_t us_pre, us_invoke_min, us_invoke_max, us_invoke_avg, us_decode;
	uint32_t crc_match; /* bit i: output map i CRC == host */
	int32_t  max_err;   /* max |dx|+|dy| px over host-confident torso keypoints, -1 if none */
	int32_t  person;    /* tr_pose_box() valid */
	tr_box_t box;
} frame_res_t;

typedef struct {
	uint32_t    magic, cpuid, vtor, heartbeat; /* flash-jlink-hp.sh beacon */
	uint32_t    stage;                         /* last stage reached, see main() */
	int32_t     status;                        /* alp_status_t of the last failure, 0 ok */
	uint32_t    cpu_mhz;
	uint32_t    verdict; /* 0 running, 1 PASS, 2 FAIL */
	frame_res_t pass[2][TR_NPU_FRAMES_MAX];
	uint32_t    sustained_hz_x10; /* pre + invoke + decode loop rate, pass B (or A) */
} results_t;

static volatile results_t *const R = (volatile results_t *)RESULT_ADDR;

static uint8_t model_sram[MODEL_MAX] __aligned(16) __attribute__((section("SRAM0")));
static uint8_t arena[ARENA_BYTES] __aligned(16) __attribute__((section("SRAM0")));

static uint32_t us(timing_t a, timing_t b)
{
	return (uint32_t)(timing_cycles_to_ns(timing_cycles_get(&a, &b)) / 1000u);
}

static int frame_pass(alp_inference_t *inf, const tr_npu_payload_t *p, uint32_t f, frame_res_t *out)
{
	const uint8_t         *base = (const uint8_t *)p;
	const tr_npu_expect_t *ex   = (const tr_npu_expect_t *)(base + p->expect_off) + f;
	alp_inference_tensor_t in = { 0 }, o[4] = { 0 };

	if (alp_inference_get_input(inf, 0, &in) != ALP_OK ||
	    in.size_bytes != TR_MN_IN * TR_MN_IN * 3) {
		return -1;
	}
	timing_t t0 = timing_counter_get();
	tr_movenet_input(base + p->frame_off[f], (int16_t)p->frame_w, (int16_t)p->frame_h, in.data);
	timing_t t1 = timing_counter_get();

	uint32_t mn = UINT32_MAX, mx = 0, sum = 0;
	for (int i = 0; i < N_INVOKE; i++) {
		timing_t     a  = timing_counter_get();
		alp_status_t st = alp_inference_invoke(inf);
		timing_t     b  = timing_counter_get();

		if (st != ALP_OK) {
			R->status = st;
			return -2;
		}
		uint32_t d = us(a, b);
		mn         = d < mn ? d : mn;
		mx         = d > mx ? d : mx;
		sum += d;
	}
	for (int i = 0; i < 4; i++) {
		if (alp_inference_get_output(inf, i, &o[i]) != ALP_OK) {
			return -3;
		}
	}
	/* Output order is the cut model's: centre, heat, offset, regress. */
	tr_movenet_out_t mo = { o[0].data, o[1].data, o[2].data, o[3].data };
	tr_pose_t        pose;
	timing_t         t2 = timing_counter_get();
	tr_movenet_decode(&mo, (int16_t)p->frame_w, (int16_t)p->frame_h, &pose);
	timing_t t3 = timing_counter_get();

	out->us_pre        = us(t0, t1);
	out->us_invoke_min = mn;
	out->us_invoke_max = mx;
	out->us_invoke_avg = sum / N_INVOKE;
	out->us_decode     = us(t2, t3);
	out->crc_match     = 0;
	for (int i = 0; i < 4; i++) {
		out->crc_match |= (crc32_ieee(o[i].data, o[i].size_bytes) == ex->map_crc[i]) << i;
	}
	out->max_err = -1;
	for (int k = TR_KP_LSHO; k <= TR_KP_RHIP; k++) {
		if (k == TR_KP_LSHO + 2) {
			k = TR_KP_LHIP;
		}
		if (ex->kp[k].score >= TR_POSE_KP_MIN) {
			int32_t e    = abs(pose.kp[k].x - ex->kp[k].x) + abs(pose.kp[k].y - ex->kp[k].y);
			out->max_err = e > out->max_err ? e : out->max_err;
		}
	}
	out->box    = tr_pose_box(&pose);
	out->person = out->box.valid;
	printk("  frame %u: pre %u us, invoke %u/%u/%u us (min/avg/max), decode %u us, crc %x, torso "
	       "err %d px,"
	       " person %d (host %d)\n",
	       f,
	       out->us_pre,
	       mn,
	       out->us_invoke_avg,
	       mx,
	       out->us_decode,
	       out->crc_match,
	       out->max_err,
	       out->person,
	       ex->person);
	return (out->person == ex->person && out->max_err <= 27) ? 0 : 1;
}

static int run_pass(int pass, const tr_npu_payload_t *p, const void *model)
{
	alp_inference_config_t cfg = {
		.model_data  = model,
		.model_size  = p->model_len,
		.format      = ALP_INFERENCE_MODEL_VELA,
		.backend     = ALP_INFERENCE_BACKEND_AUTO,
		.arena_bytes = sizeof(arena),
		.arena       = arena,
	};
	printk("pass %c: model @ %p\n", 'A' + pass, model);
	alp_inference_t *inf = alp_inference_open(&cfg);

	if (inf == NULL) {
		R->status = alp_last_error();
		printk("RESULT FAIL: alp_inference_open (pass %c) -- %s\n",
		       'A' + pass,
		       alp_status_name(alp_last_error()));
		return -1;
	}
	int bad = 0;
	for (uint32_t f = 0; f < p->n_frames && f < TR_NPU_FRAMES_MAX; f++) {
		frame_res_t r = { 0 };
		int         e = frame_pass(inf, p, f, &r);

		memcpy((void *)&R->pass[pass][f], &r, sizeof(r));
		if (e < 0) {
			printk("RESULT FAIL: frame %u pass %c stage error %d -- %s\n",
			       f,
			       'A' + pass,
			       e,
			       alp_status_name(R->status));
			alp_inference_close(inf);
			return -1;
		}
		bad += e;
	}

	/* Sustained rate on the person frame: pre + invoke + decode, 1 s. */
	alp_inference_tensor_t in, o[4];
	(void)alp_inference_get_input(inf, 0, &in);
	for (int i = 0; i < 4; i++) {
		(void)alp_inference_get_output(inf, i, &o[i]);
	}
	tr_movenet_out_t mo = { o[0].data, o[1].data, o[2].data, o[3].data };
	tr_pose_t        pose;
	uint32_t         n  = 0;
	int64_t          t0 = k_uptime_get();
	while (k_uptime_get() - t0 < 1000) {
		tr_movenet_input((const uint8_t *)p + p->frame_off[1],
		                 (int16_t)p->frame_w,
		                 (int16_t)p->frame_h,
		                 in.data);
		(void)alp_inference_invoke(inf);
		tr_movenet_decode(&mo, (int16_t)p->frame_w, (int16_t)p->frame_h, &pose);
		n++;
		R->heartbeat++;
	}
	R->sustained_hz_x10 = n * 10u;
	printk("pass %c: sustained %u Hz, %d frame(s) off host\n", 'A' + pass, n, bad);
	alp_inference_close(inf);
	return bad;
}

int main(void)
{
	const tr_npu_payload_t *p = (const tr_npu_payload_t *)TR_NPU_PAYLOAD_ADDR;

	memset((void *)R, 0, sizeof(*R));
	R->cpuid = SCB->CPUID;
	R->vtor  = SCB->VTOR;
	R->magic = RESULT_MAGIC;
	timing_init();
	timing_start();
	R->cpu_mhz = timing_freq_get_mhz();
	printk("\n=== trace-runner NPU probe (M55-HP, %u MHz) ===\n", R->cpu_mhz);

	R->stage = 1; /* payload */
	if (p->magic != TR_NPU_PAYLOAD_MAGIC || p->version != TR_NPU_PAYLOAD_VERSION ||
	    p->model_len > MODEL_MAX ||
	    crc32_ieee((const uint8_t *)p + p->model_off, p->model_len) != p->model_crc) {
		printk("RESULT FAIL: no valid payload at 0x%08x (magic %08x) -- flash payload.bin first\n",
		       TR_NPU_PAYLOAD_ADDR,
		       p->magic);
		R->verdict = 2;
		return 0;
	}
	printk("payload : %u B, model %u B (%s), %u frames %ux%u\n",
	       p->total_len,
	       p->model_len,
	       p->accel,
	       p->n_frames,
	       p->frame_w,
	       p->frame_h);

	R->stage = 2; /* pass A: model in SRAM0 */
	memcpy(model_sram, (const uint8_t *)p + p->model_off, p->model_len);
	int a = run_pass(0, p, model_sram);

	R->stage = 3; /* pass B: weights from MRAM */
	int b    = run_pass(1, p, (const uint8_t *)p + p->model_off);

	R->stage   = 4;
	R->verdict = (a == 0 && b == 0) ? 1 : 2;
	printk("RESULT %s: pass A %s, pass B %s\n",
	       R->verdict == 1 ? "PASS" : "FAIL",
	       a == 0 ? "ok" : "off/fail",
	       b == 0 ? "ok" : "off/fail");
	for (;;) {
		k_msleep(100);
		R->heartbeat++;
	}
	return 0;
}
