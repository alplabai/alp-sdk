/* echo.c -- CP-A4 stub payload: the A32 half of the frame handshake with no
 * renderer yet. Core 0 only (core 1 returns to its park, payload_start.S).
 *
 * Loop: take `in` (tr_mbox_take_in, torn reads retried); solid-fill in_fb
 * (720x1280 RGB565, Normal NC) with colour[in.tick % 8] using 128-bit NEON
 * stores, NEVER touching the TF-A MHU0 window [0x02380000, 0x02381000);
 * `dsb sy`; publish out (out_seq = in_seq, out_fb = in_fb, out_ticks0 = CNTVCT
 * ticks the fill took, out_frames++); poll ctrl_cmd for HALT between frames.
 * An in_fb that is neither TR_FB_A nor TR_FB_B is not written: out_dropped++,
 * the frame is still published so the M55 does not stall.
 *
 * Fill statistics in tr_mbox_t.pad3[] (out-block spare, payload-owned),
 * CNTVCT ticks at 100 MHz (10 ns):
 *   pad3[0] 0xEC0E0001 (echo marker)   pad3[1] min   pad3[2] max
 *   pad3[3] mean (since this LAUNCH)   pad3[4] fills counted
 *   pad3[5] in.tick of the last frame  pad3[6..7] 0
 */
#include <arm_neon.h>
#include <stdint.h>

#include "stub_abi.h"
#include "tr_mbox.h"

#define FB_BYTES    (720u * 1280u * 2u)
#define ECHO_MARKER 0xEC0E0001u

void payload_main(volatile tr_mbox_t *m);

/* white yellow cyan green magenta red blue black, RGB565 */
static const uint16_t colour[8] = {
	0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000
};

static void barrier(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}

static inline uint32_t cntvct_lo(void)
{
	uint32_t lo, hi;

	__asm__ volatile("isb\n\tmrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi)::"memory");
	(void)hi;
	return lo;
}

/* [a, b), both 64-byte aligned. */
static void fill_range(uint32_t a, uint32_t b, uint16x8_t v)
{
	for (uint16_t *p = (uint16_t *)a; p < (uint16_t *)b; p += 32) {
		vst1q_u16(p, v);
		vst1q_u16(p + 8, v);
		vst1q_u16(p + 16, v);
		vst1q_u16(p + 24, v);
	}
}

/* Whole framebuffer minus the MHU0 window (only TR_FB_B overlaps it). */
static void fill_fb(uint32_t fb, uint16_t c)
{
	uint16x8_t v   = vdupq_n_u16(c);
	uint32_t   end = fb + FB_BYTES;

	if (fb < STUB_MHU0_WINDOW + STUB_MHU0_WINDOW_SIZE && STUB_MHU0_WINDOW < end) {
		fill_range(fb, STUB_MHU0_WINDOW, v);
		fill_range(STUB_MHU0_WINDOW + STUB_MHU0_WINDOW_SIZE, end, v);
	} else {
		fill_range(fb, end, v);
	}
}

void payload_main(volatile tr_mbox_t *m)
{
	uint32_t       last = m->out_seq; /* a frame published before LAUNCH is still owed */
	uint32_t       tmin = UINT32_MAX, tmax = 0, n = 0;
	uint64_t       tsum = 0;
	tr_frame_out_t o    = { .frames = m->out_frames, .dropped = m->out_dropped };

	m->pad3[0] = ECHO_MARKER;
	for (uint32_t i = 1; i < 8u; i++)
		m->pad3[i] = 0;

	for (;;) {
		tr_frame_in_t in;
		uint32_t      fb, seq;

		if (m->ctrl_cmd == STUB_CMD_HALT) { /* consumer clears */
			m->ctrl_cmd = STUB_CMD_NONE;
			barrier();
			return;
		}
		m->out_heartbeat = ++o.heartbeat;
		if (!tr_mbox_take_in(m, last, &in, &fb, &seq, barrier)) {
			__asm__ volatile("wfe" ::: "memory"); /* event stream: ~0.66 ms poll */
			continue;
		}

		uint32_t t = 0;
		if (fb == TR_FB_A || fb == TR_FB_B) {
			uint32_t t0 = cntvct_lo();
			fill_fb(fb, colour[in.tick % 8u]);
			barrier(); /* pixels reach SRAM0 before out_seq says so (plan sec 3) */
			t    = cntvct_lo() - t0;
			tmin = t < tmin ? t : tmin;
			tmax = t > tmax ? t : tmax;
			tsum += t;
			n++;
			m->pad3[1] = tmin;
			m->pad3[2] = tmax;
			m->pad3[3] = (uint32_t)(tsum / n);
			m->pad3[4] = n;
		} else {
			o.dropped++;
		}
		m->pad3[5] = in.tick;

		o.fb     = fb;
		o.ticks0 = t;
		o.ticks1 = 0;
		o.frames++;
		tr_mbox_publish_out(m, &o, seq, barrier);
		last = seq;
	}
}
