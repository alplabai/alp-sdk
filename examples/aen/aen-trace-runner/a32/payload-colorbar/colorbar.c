/* colorbar.c -- CP-A2 test payload: NC-fill TR_FB_A AND TR_FB_B (720x1280
 * RGB565) with 8 vertical bars, EXCEPT the TF-A MHU0 payload window
 * [0x02380000, 0x02381000) inside FB B (rows 1092-1094, partially) which is
 * never written (stub_abi.h hard rule); only READ three words there, then
 * publish and heartbeat until HALT. The bar order rotates by out_seq (FB B is
 * 4 bars further on), so each HALT/LAUNCH cycle is visibly a new run.
 *
 * Record in tr_mbox_t.pad3[] (the out-block spare words, payload-owned):
 *   pad3[0]    progress: 0xCB0B0001 FB A filled, ...02 FB B filled,
 *              ...03 window read back (a stuck value locates a hang/abort)
 *   pad3[1..3] words read at MHU_PROBE[0..2] (read only, never written)
 *   pad3[4..6] 0 (reserved)
 *   pad3[7]    ISR after the reads (bit 8 A = asynchronous abort pending:
 *              CPSR.A is masked, so an external abort never reaches a vector)
 * A synchronous abort on a read lands in the stub vectors: stub_state=FAULT,
 * fault_code DABORT, DFAR = the address, progress stays 0xCB0B0002.
 * A read that returns without an abort says NS A32 can at least read the
 * window; whether writes stick is deliberately NOT tested (they could corrupt
 * TF-A/SE messaging).
 */
#include <stdint.h>

#include "stub_abi.h"
#include "tr_mbox.h"

#define FB_W        720u
#define FB_H        1280u
#define BARS        8u
#define BAR_WORDS   (FB_W / BARS / 2u) /* 90 px = 45 words per bar */
#define PROGRESS    0xCB0B0000u

void payload_main(volatile tr_mbox_t *m);

/* white yellow cyan green magenta red blue black, RGB565 */
static const uint16_t colour[BARS] = { 0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000 };
static const uint32_t mhu_probe[3] = { 0x02380000u, 0x02380800u, 0x02380FFCu };

static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static uint32_t bar_word(uint32_t bar, uint32_t rot)
{
	uint32_t c = colour[(bar + rot) % BARS];

	return c | c << 16;
}

/* Normal NC in the stub table: plain stores, no cache maintenance. Words in
 * [STUB_MHU0_WINDOW, +STUB_MHU0_WINDOW_SIZE) are skipped exactly. */
static void fill(uint32_t fb, uint32_t rot)
{
	uint32_t *p = (uint32_t *)fb;

	for (uint32_t y = 0; y < FB_H; y++)
		for (uint32_t b = 0; b < BARS; b++) {
			uint32_t c = bar_word(b, rot);
			for (uint32_t x = 0; x < BAR_WORDS; x++, p++)
				if ((uint32_t)p - STUB_MHU0_WINDOW >= STUB_MHU0_WINDOW_SIZE)
					*p = c;
		}
}

void payload_main(volatile tr_mbox_t *m)
{
	uint32_t seq = m->out_seq + 1u;
	uint32_t rot_b = seq + BARS / 2u;

	fill(TR_FB_A, seq);
	dsb();
	m->pad3[0] = PROGRESS | 1u;
	fill(TR_FB_B, rot_b);
	dsb();
	m->pad3[0] = PROGRESS | 2u;

	for (uint32_t i = 0; i < 3u; i++) {
		m->pad3[1u + i] = *(const volatile uint32_t *)mhu_probe[i];
		m->pad3[4u + i] = 0;
	}
	uint32_t isr;
	__asm__ volatile("isb\n\tmrc p15, 0, %0, c12, c1, 0" : "=r"(isr)::"memory");
	m->pad3[7] = isr;
	m->pad3[0] = PROGRESS | 3u;

	dsb(); /* pixels reach SRAM0 before out_seq says so (plan sec 3) */
	m->out_fb = TR_FB_A;
	m->out_frames = m->out_frames + 1u;
	dsb();
	m->out_seq = seq;
	dsb();

	for (;;) {
		m->out_heartbeat = m->out_heartbeat + 1u;
		if (m->ctrl_cmd == STUB_CMD_HALT) { /* consumer clears */
			m->ctrl_cmd = STUB_CMD_NONE;
			dsb();
			return;
		}
		__asm__ volatile("wfe" ::: "memory"); /* stub left the CNTKCTL event stream on */
	}
}
