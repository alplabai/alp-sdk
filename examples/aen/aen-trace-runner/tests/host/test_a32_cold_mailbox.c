/* tests/host/test_a32_cold_mailbox.c -- a power-on-garbage mailbox page
 * (SRAM1 is uninitialised on a cold boot) must never present the renderer
 * with a waiting frame, and the renderer's rotation ABI check binds only
 * frames it will draw (bench: a release self-launched ~120 ms after TF-A read
 * a garbage "frame", the rotation check trapped, and the HE watchdog had to
 * relaunch it). Also: the launch token the core-1 gate waits for is never 0.
 *   1. a page full of garbage: tr_mbox_cold_clear() leaves in_seq == out_seq == 0,
 *      the renderer's first poll (`last = out_seq`) finds nothing to take, and
 *      identity / control words outside [in_seq, ctrl_cmd) are untouched;
 *   1b. the stub's warm/cold decision: cold or old-version pages are cleared, a warm page of
 *      this version is kept;
 *   2. a real frame published after the clear is taken, rotation intact;
 *   3. tr_rot_refuse(): a bad rotation is refused only on a drawn frame;
 *   4. stub_next_token(): never 0, wraps to 1, differs from its input. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../a32/common/stub_abi.h"
#include "../../src/ipc/tr_mbox.h"
#include "../../src/render/panel_rot.h"

static volatile tr_mbox_t m;

static void barrier(void)
{
}

int main(void)
{
	/* 1. Cold page: garbage everywhere, rotation byte included. */
	memset((void *)&m, 0xA7, sizeof(m));
	m.magic       = 0x12345678u; /* the caller's: identity and control survive the clear */
	m.version     = 0x9ABCDEF0u;
	m.ctrl_entry  = 0x02500000u;
	m.in_seq      = 0xA7A7A7A7u;
	m.out_seq     = 0x13572468u;
	m.in.rotation = 0xA7A7u;
	tr_mbox_cold_clear(&m);
	assert(m.in_seq == 0u && m.out_seq == 0u && m.in_fb == 0u && m.in.rotation == 0u);
	assert(m.out_dropped == 0u && m.out_frames == 0u && m.pad3[7] == 0u);
	assert(m.magic == 0x12345678u && m.version == 0x9ABCDEF0u && m.ctrl_entry == 0x02500000u);
	assert(m.ctrl_cmd == 0xA7A7A7A7u && m.stub_state == 0xA7A7A7A7u); /* not ours to clear here */
	{
		uint32_t      last = m.out_seq; /* renderer_main(): a frame before LAUNCH is owed */
		tr_frame_in_t in;
		uint32_t      fb, seq;

		assert(!tr_mbox_take_in(&m, last, &in, &fb, &seq, barrier)); /* nothing waiting */
	}

	/* 1b. The stub's decision at its call site (tr_mbox_stub_page_init): cold garbage and a
	 * warm page of ANOTHER mailbox version are cleared; a warm page of this version is
	 * kept, frame owed and all. Mutating the guard (magic only, or inverted) fails here. */
	memset((void *)&m, 0xA7, sizeof(m));
	assert(tr_mbox_stub_page_init(&m) == 0 && m.in_seq == 0u && m.out_seq == 0u);
	memset((void *)&m, 0xA7, sizeof(m));
	m.magic   = TR_MBOX_MAGIC;
	m.version = TR_MBOX_VERSION - 1u; /* an older stub's page: other offsets */
	assert(tr_mbox_stub_page_init(&m) == 0);
	assert(m.in_seq == 0u && m.out_seq == 0u && m.in.rotation == 0u);
	memset((void *)&m, 0xA7, sizeof(m));
	m.magic   = TR_MBOX_MAGIC;
	m.version = TR_MBOX_VERSION;
	m.in_seq  = 7u;
	m.out_seq = 6u;
	assert(tr_mbox_stub_page_init(&m) == 1 && m.in_seq == 7u && m.out_seq == 6u); /* kept */

	tr_mbox_cold_clear(&m); /* back to a clean page for the frame below */
	/* 2. The HE's first real frame after the clear is taken, rotation intact. */
	{
		tr_frame_in_t f, in;
		uint32_t      fb, seq;

		memset(&f, 0, sizeof(f));
		f.rotation = 270;
		tr_mbox_publish_in(&m, &f, TR_FB_A, barrier);
		assert(tr_mbox_take_in(&m, 0u, &in, &fb, &seq, barrier));
		assert(in.rotation == 270 && fb == TR_FB_A && seq == 1u);
	}

	/* 3. The rotation ABI binds drawn frames only. */
	assert(!tr_rot_refuse(1, 0) && !tr_rot_refuse(1, 90) && !tr_rot_refuse(1, 270));
	assert(tr_rot_refuse(1, 180) && tr_rot_refuse(1, 0xA7A7u) && tr_rot_refuse(1, 14));
	assert(!tr_rot_refuse(0, 0xA7A7u) &&
	       !tr_rot_refuse(0, 180)); /* undrawn: dropped, not a fault */

	/* 4. The core-1 gate token. */
	assert(stub_next_token(0u) == 1u && stub_next_token(1u) == 2u);
	assert(stub_next_token(0xFFFFFFFFu) == 1u); /* wraps past 0 */
	for (unsigned h = 0; h < 5; h++) {
		assert(stub_next_token(h) != 0u && stub_next_token(h) != h);
	}

	puts("a32 cold mailbox ok");
	return 0;
}
