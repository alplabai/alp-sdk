/*
 * src/platform/a32.c -- the M55 end of the A32 renderer link.
 *
 * Protocol: docs/superpowers/plans/2026-09-22-a32-renderer.md section 6; wire
 * layout: src/ipc/tr_mbox.h. The M55 writes the identity block and `in`, reads
 * `out`, and uses the stub control block only to HALT (and, in the release
 * flow, LAUNCH). The mailbox is uncached on this side (CONFIG_DCACHE=n), so a
 * DSB is the only ordering step.
 */
#include <cmsis_core.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/printk.h>

#include "../ipc/tr_flip.h"
#include "../ipc/tr_memmap.h" /* TR_MEM_SRAM1_READY -- HP camera-pool gate, see tr_a32_boot() */
#include "../ipc/tr_wd.h"
#include "../render/r3d.h" /* TR_R3D_W: the widest panel the renderer takes */
#include "a32.h"
#include "display.h"

#if TR_M55_AUTOLAUNCH
/* Build-time header for the release flow: TR_A32_ENTRY, TR_A32_LEN, TR_A32_CRC
 * (CRC-32 the stub checks over [entry, entry + len)). CMakeLists.txt passes
 * its path as TR_A32_LAUNCH_H. */
#include TR_A32_LAUNCH_H
/* Marker + the payload identity this image LAUNCHes, in .rodata so the
 * release packaging (a32/release/build-release.sh) can check an HE image
 * against the renderer it ships with (nm + objdump, no guessing). */
const volatile uint32_t tr_a32_autolaunch_id[3] = { TR_A32_ENTRY,
	                                                TR_A32_LEN,
	                                                TR_A32_CRC }; /* volatile: read from ROM */
#endif

#define TR_OUT_TIMEOUT_MS 100 /* no out_seq this long: the A32 is late or gone */
/* Poll period while waiting for out_seq: 100 us (one tick at
 * CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000, pinned in prj.conf and asserted
 * below). A 1 ms poll cost up to 1 ms of the
 * 25 ms frame on every flip -- measured ~5 % missed vsyncs at a 22 ms A32
 * frame. */
#define TR_POLL_US 100
BUILD_ASSERT(CONFIG_SYS_CLOCK_TICKS_PER_SEC >= 1000000 / TR_POLL_US,
             "the 100 us polls need a >= 10 kHz tick");
/* Boot: how long tr_a32_boot() waits for the stub to come up (mailbox magic)
 * and settle before the game starts; past it the watchdog keeps checking at
 * every missed frame, so a later stub is still LAUNCHed. 2026W36-0009 cold boots
 * (v5.1, 2026-09-24): the stub came up more than 3 s after the HE -- the
 * boot logs the real figure ("stub alive N ms after ..."). */
#define TR_STUB_BOOT_WAIT_MS 15000
/* A32 stamp deltas above this are garbage (an older stub/renderer that never
 * wrote them): the first-frame breakdown is not printed. */
#define TR_STAMP_MAX_US 10000000u

static volatile tr_mbox_t *const g_mbox = (volatile tr_mbox_t *)TR_MBOX_ADDR;

/* Bench-readable over the AHB-AP (non-static so `nm` names them). */
volatile uint32_t tr_a32_timeouts;     /* frames the A32 did not finish in TR_OUT_TIMEOUT_MS */
volatile uint32_t tr_in_seq_published; /* in_seq of the last frame handed to the A32 */
volatile uint32_t
    tr_flip_dropped; /* finished frames never shown: bad out_fb or swap never landed */
volatile uint32_t
    tr_a32_relaunches; /* watchdog LAUNCH/HALT commands sent this boot (TR_M55_AUTOLAUNCH) */
volatile uint32_t
    tr_a32_first_frame_us;           /* last LAUNCH (or stub first seen) -> first frame, HE clock */
volatile uint32_t tr_a32_pending_ms; /* how long the command in flight has waited (0: none) */
/* Sum of out_ticks0 / out_ticks1 (CNTVCT, 100 MHz) over every frame that
 * landed: each A32 core's busy time, for the HUD perf panel (hud_l2.c). */
volatile uint64_t tr_a32_busy_ticks[2];

static uint32_t g_out_seen;  /* last out_seq taken */
static tr_wd_t  g_wd;        /* src/ipc/tr_wd.h: first-frame / stall / backoff rules */
static bool     g_link_ok;   /* SRAM1 answered: the mailbox may be touched */
static bool     g_in_flight; /* a frame has been published this boot */
static bool     g_timeout_printed;
static bool     g_drop_printed;

static void dsb(void)
{
	barrier_dsync_fence_full();
}

/*
 * TR_MEM_SRAM1_READY starts CLEARED, not whatever it happened to read at
 * power-on (fix round 3, reviewer): SRAM0 is always-on on this board, so a
 * warm reset (no power cycle) leaves a PREVIOUS boot's ready magic sitting
 * there -- if THIS boot's tr_a32_boot() has not run yet (or is still
 * retrying sram1_answers()), the HP could read a stale "ready" from the
 * last boot and touch CAM_POOL (SRAM1) before this boot has confirmed it
 * itself. PRE_KERNEL_1, priority 0: the earliest init this core runs, before
 * anything on it could plausibly race the HP's reads (the HP's own first bus
 * touch, tr_i2c1_unstick_init(), is POST_KERNEL priority 1, behind its
 * alp,i2c-handover wait).
 */
static int tr_sram1_ready_clear_init(void)
{
	*(volatile uint32_t *)TR_MEM_SRAM1_READY = 0u;
	return 0;
}
SYS_INIT(tr_sram1_ready_clear_init, PRE_KERNEL_1, 0);

static uint64_t now_us(void)
{
	return k_cyc_to_us_floor64(k_cycle_get_64());
}

/* CNTVCT (100 MHz) tick delta -> us. */
static uint32_t ticks_us(uint32_t from, uint32_t to)
{
	return (to - from) / 100u;
}

/* The first frame since boot / the last LAUNCH landed: how long it took, HE
 * side, and where the A32 spent it (the stub's and renderer's stamps,
 * tr_mbox.h TR_STUB_T_* / TR_RENDER_T_ADDR). */
static void log_first_frame(uint64_t waited_us)
{
	const volatile uint32_t *st = g_mbox->pad4;
	const volatile uint32_t *rt = (const volatile uint32_t *)TR_RENDER_T_ADDR;

	tr_a32_first_frame_us = (uint32_t)waited_us;
	printk("a32     : first frame %u ms after %s\n",
	       (unsigned)(waited_us / 1000u),
	       g_wd.last_cmd == TR_CTRL_LAUNCH ? "our LAUNCH" : "the stub came up");

	/* Only a stub and renderer that wrote their stamps this LAUNCH: marker,
	 * all four set, every span plausible. Anything else prints nothing. */
	uint32_t copy  = st[TR_STUB_T_COPY0] ? ticks_us(st[TR_STUB_T_COPY0], st[TR_STUB_T_COPY1]) : 0u;
	uint32_t crc   = ticks_us(st[TR_STUB_T_LAUNCH], st[TR_STUB_T_JUMP]);
	uint32_t init  = ticks_us(rt[0], rt[1]);
	uint32_t frame = ticks_us(rt[1], rt[2]);
	uint32_t total = ticks_us(st[TR_STUB_T_LAUNCH], rt[2]);

	if (*(const volatile uint32_t *)TR_RENDER_STATS_ADDR != TR_RENDER_STATS_MARKER || rt[0] == 0u ||
	    rt[1] == 0u || rt[2] == 0u || st[TR_STUB_T_LAUNCH] == 0u || st[TR_STUB_T_JUMP] == 0u ||
	    copy > TR_STAMP_MAX_US || crc > TR_STAMP_MAX_US || init > TR_STAMP_MAX_US ||
	    frame > TR_STAMP_MAX_US || total > TR_STAMP_MAX_US) {
		return;
	}
	printk("a32     : A32 us: MRAM copy %u, CRC+sync %u, jump->init done %u, init->first frame %u, "
	       "launch->first frame %u\n",
	       (unsigned)copy,
	       (unsigned)crc,
	       (unsigned)init,
	       (unsigned)frame,
	       (unsigned)total);
}

/* The stub initialised the page this boot (or an earlier one): stub_state
 * and ctrl_cmd are real, not power-on garbage. */
static bool stub_alive(void)
{
	if (g_mbox->magic != TR_MBOX_MAGIC) {
		return false;
	}
	/* A stub of another mailbox layout cannot be driven: this HE's frames are
	 * TR_MBOX_VERSION, so it is not "alive" for us (reported once). */
	if (g_mbox->version != TR_MBOX_VERSION) {
		static bool warned;

		if (!warned) {
			warned = true;
			printk("a32     : stub speaks mailbox version %u, this HE %u -- not driving it\n",
			       (unsigned)g_mbox->version,
			       (unsigned)TR_MBOX_VERSION);
		}
		return false;
	}
	return true;
}

#if TR_M55_AUTOLAUNCH
/* The mailbox's ctrl_entry/len/crc name the image this HE LAUNCHes (the
 * release stub self-LAUNCHes with its own header's values). */
static bool stub_image_ours(void)
{
	return g_mbox->ctrl_entry == tr_a32_autolaunch_id[0] &&
	       g_mbox->ctrl_len == tr_a32_autolaunch_id[1] &&
	       g_mbox->ctrl_crc == tr_a32_autolaunch_id[2];
}

/* One watchdog decision (tr_wd_poll): write the command it asks for and log
 * why; a no-command reason is logged when it changes, a command in flight
 * once a second (tr_wd.h known limits). */
static void wd_poll(bool missed)
{
	static tr_wd_why_t last_why = TR_WD_WHY_N;
	static uint32_t    last_pending_s;
	tr_wd_why_t        why;
	uint32_t           state = g_mbox->stub_state, ctrl = g_mbox->ctrl_cmd;
	uint64_t           now = now_us();
	uint32_t           cmd =
	    tr_wd_poll(&g_wd, stub_alive(), state, ctrl, stub_image_ours(), missed, now, &why);
	uint32_t pending_ms = (uint32_t)(tr_wd_pending_us(&g_wd, now) / 1000u);

	tr_a32_pending_ms = pending_ms;
	if (pending_ms / 1000u != last_pending_s && pending_ms >= 1000u) {
		printk("a32     : watchdog: %s for %u ms (stub_state=%u ctrl_cmd=%u)%s\n",
		       tr_wd_why_str(why),
		       (unsigned)pending_ms,
		       state,
		       ctrl,
		       pending_ms >= 5000u ? " -- the A32 may be wedged: power-cycle" : "");
	}
	last_pending_s = pending_ms / 1000u;

	if (cmd != TR_CTRL_NONE) {
		if (cmd == TR_CTRL_LAUNCH &&
		    (g_mbox->fault_code != 0u || (g_mbox->pad3[7] >> 16) == 0xAB1Du)) {
			/* A LAUNCH clears the stub's fault record: leave it on the console first. */
			printk("a32     : fault record before relaunch: core %u code %u lr 0x%08x dfsr "
			       "0x%08x dfar 0x%08x ifsr 0x%08x ifar 0x%08x abi 0x%08x\n",
			       g_mbox->fault_core,
			       g_mbox->fault_code,
			       g_mbox->lr,
			       g_mbox->dfsr,
			       g_mbox->dfar,
			       g_mbox->ifsr,
			       g_mbox->ifar,
			       g_mbox->pad3[7]);
			if (tr_abi_fault_code(g_mbox->pad3[7]) == TR_ABI_FAULT_FW) {
				printk("a32     : renderer refused fw=%u (a panel width must be a multiple of 16, "
				       "16..%u)\n",
				       (unsigned)tr_abi_fault_arg(g_mbox->pad3[6]),
				       (unsigned)TR_R3D_W);
			} else if (tr_abi_fault_code(g_mbox->pad3[7]) == TR_ABI_FAULT_ROTATION) {
				printk("a32     : renderer refused rotation=%u (0, 90 or 270)\n",
				       (unsigned)tr_abi_fault_arg(g_mbox->pad3[6]));
			} else if (tr_abi_fault_code(g_mbox->pad3[7]) == TR_ABI_FAULT_VERSION) {
				printk("a32     : renderer faulted on mailbox version %u (this HE %u)\n",
				       (unsigned)tr_abi_fault_arg(g_mbox->pad3[6]),
				       (unsigned)TR_MBOX_VERSION);
			}
			g_mbox->pad3[6] = 0u;
			g_mbox->pad3[7] = 0u; /* said once: a stale record is not repeated */
		}
		if (cmd == TR_CTRL_LAUNCH) {
			g_mbox->ctrl_entry = tr_a32_autolaunch_id[0];
			g_mbox->ctrl_len   = tr_a32_autolaunch_id[1];
			g_mbox->ctrl_crc   = tr_a32_autolaunch_id[2];
			dsb();
		}
		g_mbox->ctrl_cmd = cmd;
		dsb();
		tr_a32_relaunches++;
		printk("a32     : watchdog %s #%u: %s (stub_state=%u, launches %u",
		       cmd == TR_CTRL_LAUNCH ? "LAUNCH" : "HALT",
		       tr_a32_relaunches,
		       tr_wd_why_str(why),
		       state,
		       g_wd.tries);
		if (why == TR_WD_HALT_FOREIGN) {
			printk(", running entry 0x%08x len %u crc 0x%08x",
			       g_mbox->ctrl_entry,
			       g_mbox->ctrl_len,
			       g_mbox->ctrl_crc);
		}
		if (cmd == TR_CTRL_LAUNCH) {
			printk(", first frame due in %u ms",
			       (unsigned)((tr_wd_deadline_us(&g_wd) - now) / 1000u));
		}
		printk(")\n");
	} else if (why != last_why) {
		printk("a32     : watchdog: %s (stub_state=%u)\n", tr_wd_why_str(why), state);
	}
	last_why = why;
}
#endif

/*
 * SRAM1 holds the mailbox and is powered only once the A32 chain (TF-A)
 * has run; an M55-only boot, or an HE that starts before TF-A got there,
 * bus-faults on the first access. Probe with the fault ignored: FAULTMASK
 * raises the priority to -1, CCR.BFHFNMIGN makes data bus faults at that
 * priority ignored, and BFSR still records a precise one.
 *
 * ponytail: probe only. Asking the SE to power SRAM1 itself
 * (SERVICES_power_memory_req(POWER_MEM_SRAM_1_ENABLE)) is not reachable
 * from this image: hal_alif's Zephyr SE client has no memory-power call
 * and is not built here (HAS_ALIF_SE_SERVICES needs ARM_MHUV2 + the SE
 * services DT node). Add it if a boot ever waits the full budget.
 */
static bool sram1_answers(void)
{
	uint32_t ccr = SCB->CCR, fm = __get_FAULTMASK();
	/* Earlier fault records stay: the probe judges and clears only the
	 * BFSR bits it sets. Only if a bus fault is ALREADY recorded (the probe
	 * would be blind to its own) is BFSR cleared first -- and said so. */
	uint32_t before = SCB->CFSR & SCB_CFSR_BUSFAULTSR_Msk;

	if (before != 0u) {
		printk("a32     : clearing an earlier BFSR record 0x%02x (BFAR 0x%08x) to probe SRAM1\n",
		       (unsigned)(before >> SCB_CFSR_BUSFAULTSR_Pos),
		       (unsigned)SCB->BFAR);
		SCB->CFSR = before;
		before    = 0u;
	}
	__set_FAULTMASK(1);
	SCB->CCR = ccr | SCB_CCR_BFHFNMIGN_Msk;
	__DSB();
	__ISB();
	(void)*(volatile uint32_t *)TR_MBOX_ADDR;
	__DSB();
	__ISB();

	uint32_t bfsr = (SCB->CFSR & SCB_CFSR_BUSFAULTSR_Msk) & ~before;

	SCB->CFSR = bfsr; /* write-1-to-clear: just the probe's bits */
	SCB->CCR  = ccr;
	__DSB();
	__ISB();
	__set_FAULTMASK(fm);
	return bfsr == 0u;
}

#define TR_SRAM1_WAIT_MS  2000 /* A32 chain + TF-A power SRAM1 well inside this */
#define TR_SRAM1_RETRY_MS 10

void tr_a32_boot(void)
{
	/*
	 * CONFIG_DCACHE=n does not turn the D-cache OFF: Zephyr only disables it
	 * at boot when CONFIG_DCACHE=y, so a warm RAM-run after a TR_RENDER=M55
	 * image inherits CCR.DC=1. The mailbox is immune either way -- the board
	 * overlay makes all of SRAM1 an MPU NOCACHE region -- so this only says
	 * so. Not disabled here: that needs a set/way clean loop, and the
	 * set/way loop in SCB_EnableDCache is known to hang on this E8.
	 */
	if (SCB->CCR & SCB_CCR_DC_Msk) {
		printk("a32     : D-cache left ON by a previous image (mailbox is MPU non-cacheable)\n");
	}

	printk("a32     : mailbox at %p\n", (void *)g_mbox);
	for (int waited = 0; !sram1_answers(); waited += TR_SRAM1_RETRY_MS) {
		if (waited >= TR_SRAM1_WAIT_MS) {
			printk("RESULT FAIL: SRAM1 (A32 mailbox) not powered after %d ms -- no A32 chain? "
			       "Panel stays up, nothing is rendered\n",
			       TR_SRAM1_WAIT_MS);
			/* Defensive, not just the SYS_INIT clear above (fix round 3):
			 * if this confirmation genuinely failed, TR_MEM_SRAM1_READY
			 * must not still read as ready from some earlier success this
			 * same power cycle (e.g. a prior tr_a32_boot() call). */
			*(volatile uint32_t *)TR_MEM_SRAM1_READY = 0u;
			dsb();
			return; /* g_link_ok stays false: present/flush never touch SRAM1 */
		}
		k_msleep(TR_SRAM1_RETRY_MS);
	}
	g_link_ok = true;

	/*
	 * HP camera-pool gate (design fix round 2, hp_vision/src/main.c):
	 * sram1_answers() above IS the confirmation that SRAM1 is really
	 * powered and readable, the same fact tr_a32_link_ok() reports to
	 * the rest of this file -- write it where the OTHER M55 core can
	 * see it without ever touching SRAM1 itself, since the whole point
	 * is the HP must not have to probe the very memory it is unsure is
	 * safe to touch. TR_MEM_SRAM1_READY is SRAM0 (this board's SoC
	 * DFP has no readable "SRAM1 is powered" status bit -- VBAT.RET_CTRL
	 * (Device/soc/AE822FA0E5597/include/rtss_hp/soc.h, AIPM's
	 * MB_SRAM1_RET / SRAM1_RET_MASK bit 26, se_services/include/aipm.h)
	 * is a deep-sleep RETENTION force/mask control, not bus-access
	 * telemetry, and this board's own SRAM1 bus-fault is a cold-boot/
	 * power-sequencing fact this register was never observed against on
	 * real silicon -- an unverified reinterpretation of a control bit
	 * is a worse bet than the fact this core already measured directly).
	 */
	{
		volatile uint32_t *ready = (volatile uint32_t *)TR_MEM_SRAM1_READY;

		*ready = TR_MEM_SRAM1_READY_MAGIC;
		dsb();
	}

	/* magic/version belong to the stub, which writes them at its init: a
	 * valid magic means the counters and stub_state are real, not
	 * power-on garbage. The M55 writes only its own identity field -- it
	 * never writes magic, so a cold page cannot be made to look warm. */
	bool had_magic = (g_mbox->magic == TR_MBOX_MAGIC);

	g_mbox->m55_boot_count = had_magic ? g_mbox->m55_boot_count + 1u : 1u;
	dsb();

	tr_wd_init(&g_wd, now_us());
#if TR_M55_AUTOLAUNCH
	/* Release flow: wait (bounded) for the stub, then either adopt the
	 * renderer it self-LAUNCHed -- or one left RUNNING by a previous M55
	 * boot: it takes the next in_seq like any other -- or LAUNCH a PARKED
	 * one (tr_wd.h rule 3). A renderer that never delivers is the
	 * watchdog's, at every missed frame. The dev flow does none of this:
	 * there the bench LAUNCHes. */
	int64_t t0   = k_uptime_get();
	bool    seen = false;

	for (;;) {
		uint32_t sent   = tr_a32_relaunches;
		int64_t  waited = k_uptime_get() - t0;

		if (!seen && stub_alive()) {
			seen = true;
			printk("a32     : stub alive %lld ms after the boot wait began (HE uptime %lld ms), "
			       "stub_state=%u\n",
			       (long long)waited,
			       (long long)(t0 + waited),
			       g_mbox->stub_state);
		}
		wd_poll(false);
		if (tr_a32_relaunches != sent) {
			break;
		}
		/* A foreign RUNNING payload got its HALT above (tr_wd.h rule 6):
		 * RUNNING here is ours, or a HALT is already in flight. */
		if (stub_alive() && g_mbox->stub_state == TR_STUB_RUNNING) {
			printk("a32     : renderer RUNNING after %lld ms -- adopted\n", (long long)waited);
			break;
		}
		if (waited >= TR_STUB_BOOT_WAIT_MS) {
			printk("a32     : stub not ready after %d ms (magic=0x%08x stub_state=%u) -- starting "
			       "anyway, "
			       "the watchdog LAUNCHes it when it is\n",
			       TR_STUB_BOOT_WAIT_MS,
			       g_mbox->magic,
			       g_mbox->stub_state);
			break;
		}
		k_msleep(1);
	}
#endif
	g_out_seen = g_mbox->out_seq;
	printk("a32     : m55 boot #%u stub_state=%u in_seq=%u\n",
	       g_mbox->m55_boot_count,
	       g_mbox->stub_state,
	       g_mbox->in_seq);
}

bool tr_a32_link_ok(void)
{
	return g_link_ok;
}

void tr_a32_flush(void)
{
	if (!g_link_ok || !g_in_flight) {
		return;
	}
	g_in_flight = false;

	tr_frame_out_t out;
	uint32_t       seq;

	for (int64_t deadline = k_uptime_get() + TR_OUT_TIMEOUT_MS; k_uptime_get() < deadline;) {
		if (tr_mbox_take_out(g_mbox, g_out_seen, &out, &seq, dsb)) {
			g_out_seen = seq;
			if (seq == tr_in_seq_published) {
				g_timeout_printed = false; /* recovered: the next stall logs again */
				/* Rejects anything but the free buffer, so a bad out_fb
				 * can never reach the CDC200. */
				if (tr_display_flip_to(out.fb) == 0) {
					uint64_t now = now_us(), waited = now - g_wd.wait_us;

					if (tr_wd_landed(&g_wd, now)) {
						log_first_frame(waited);
					}
					tr_a32_busy_ticks[0] += out.ticks0;
					tr_a32_busy_ticks[1] += out.ticks1;
				} else {
					tr_flip_dropped++;
					if (!g_drop_printed) {
						g_drop_printed = true;
						printk("a32     : frame %u not shown (out_fb=0x%08x)\n", seq, out.fb);
					}
				}
				return;
			}
		}
		k_usleep(TR_POLL_US);
	}

	/* Stall watchdog: the last frame stays on the glass. */
	tr_a32_timeouts++;
	if (!g_timeout_printed) {
		g_timeout_printed = true;
		printk(
		    "a32     : no out_seq for %d ms (in_seq=%u stub_state=%u) -- holding the last frame\n",
		    TR_OUT_TIMEOUT_MS,
		    tr_in_seq_published,
		    g_mbox->stub_state);
	}
#if TR_M55_AUTOLAUNCH
	/* Release flow: tr_wd.h decides -- a first frame gets its own, longer
	 * timeout; a stall after frames landed is HALTed, and every HALT is
	 * followed by a LAUNCH; retries back off, without a cap on attempts. */
	wd_poll(true);
#endif
}

void tr_a32_present(const tr_frame_in_t *in)
{
	/* The stub must have initialised the page: publishing into a cold page it has not
	 * cleared yet (the "starting anyway" boot path) would be overwritten by that clear. */
	if (!g_link_ok || !stub_alive()) {
		return;
	}
	tr_a32_flush();

	uint32_t fb = tr_display_free_fb();

	if (fb == 0u) {
		return; /* live buffer is neither FB: nothing safe to hand out */
	}
	tr_mbox_publish_in(g_mbox, in, fb, dsb);
	tr_in_seq_published = g_mbox->in_seq;
	g_in_flight         = true;
}
