/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- SPI-slave transport wiring (the
 * DEFAULT host-control link).  Built ONLY for CC3501E_HAL_BACKEND=ti.
 *
 * Drives the SILICON-FREE byte seams in src/transport_spi.c from the
 * CC3501E's SPI peripheral via TI Drivers (SPI_open in SPI_PERIPHERAL +
 * SPI_MODE_CALLBACK).  Inter-chip pins (metadata/e1m_modules/aen/
 * inter-chip.tsv): SCLK/MOSI/MISO on CC3501E GPIO_27/28/29; Alif is
 * master, CC3501E is slave.
 *
 * ============== radio<->bridge coexistence (SHIP-CRITICAL) ============
 * The bridge SPI shares the CC35xx host-DMA controller with the on-chip
 * Wi-Fi/BLE radio.  The radio is a separate subsystem (NWP), but its HIF
 * bring-up runs on the AP core and re-inits the DMA block GLOBALLY: when
 * Wlan_Start (or a short Wlan_Get) runs, the bridge SPI's DMA transfer
 * stops completing and MISO goes silent -- bench-proven, and it happens
 * REGARDLESS of which DMA channel the bridge is pinned to (we tried ch11,
 * ch8/6, and the FREE ch12/13; all die the same way).  READY gates
 * command/reply phases once the slave is re-armed, but there is not yet a
 * HOST_IRQ async-event line for the CC35 to initiate traffic, so the bridge
 * simply cannot be serviced WHILE a radio op runs.
 *
 * The architecture that works WITH this constraint (submit -> radio-op
 * [bridge down] -> recover -> poll):
 *   1. Wlan_Start runs ONCE at boot, before any host traffic
 *      (cc3501e_hw_wifi_boot_start, main.c bringup_task).
 *   2. After EVERY radio op the worker calls bridge_transport_spi_hw_reinit()
 *      (worker.c / cc3501e_hw_ti.c) which does a REAL SPI_close + SPI_open +
 *      re-arm here, restoring the slave once the op is done.
 *   3. The host poll-retries on ALP_ERR_IO (link down during the op) with a
 *      budget > the op duration (chips/cc3501e/cc3501e.c poll_by_repeat).
 * The bridge DMA is pinned to the FREE channels 12/13 (cc3501e_aen{,_wifi}.
 * syscfg) -- harmless, and the right place if a future SDK stops the global
 * DMA re-init; but correctness here rests on the re-open, not the channel.
 *
 * The re-open MUST run from the bring-up/worker task, NOT the SPI callback:
 * a radio op leaves the slave's DMA dead, so the in-flight transfer never
 * completes and the callback never fires -- a flag the callback would have
 * to read is therefore useless.  reinit is only ever called right after a
 * radio op (when the slave is already dead), so SPI_close does not race a
 * live callback.
 *
 * ============== HARDWARE-SS0 PHASE FRAMING (this rev) ================
 * The current E1M-AEN rev wires SCLK/MOSI/MISO plus hardware SS0 and
 * READY.  The Alif dwc-ssi master asserts/deasserts SS0 around each
 * protocol phase; the CC3501E slave advances on SPI_TRANSFER_COMPLETED
 * and raises READY after it has armed the next phase:
 *
 *   1. master clocks 4    -> request header   (slave reads payload_len)
 *   2. master clocks N    -> request payload   (N = that payload_len)
 *   3. master clocks 4    -> reply header      (master reads reply len)
 *   4. master clocks M    -> reply payload      (M = that reply len)
 *
 * The completed request frame is replayed through the byte seams, so
 * framing/dispatch (and the host test) are identical to the stub path.
 * HOST_IRQ / async-event push delivery remains future work; solicited
 * command traffic uses the hardware-SS0 + READY path here.
 *
 * ================= OTA UPDATE MODE (polled bridge) ==================
 * A callback/DMA SPI_open PERMANENTLY prevents psa_fwu_start() and
 * psa_fwu_write() from returning -- bench-proven on silicon 2026-08-21: the
 * same sequence RETURNS at boot before any SPI_open, RETURNS after a POLLED
 * (SPI_MODE_BLOCKING) SPI_open, and NEVER returns after a callback/DMA one.
 * SPI_close() does not undo the claim, and SPI_transferCancel() does not return
 * from the pump context (it hung the bridge twice).  Quiescing the re-arm,
 * lowering the task priority, raising the stack, masking interrupts and
 * disabling the radio were all tried and all refuted.  So the only way to run
 * psa_fwu at all is to never open the slave in callback mode ON THAT BOOT.
 *
 * Update mode is therefore a BOOT MODE of this transport, not a new subsystem.
 * A magic word in a .TI.noinit RAM struct is armed by protocol opcode 0x47 and
 * consumed READ-AND-CLEAR on the next boot; bridge_transport_spi_polled() is
 * the single predicate everything keys off, latched for the whole boot:
 *   - spi_open_and_arm()  opens SPI_MODE_BLOCKING / SPI_WAIT_FOREVER,
 *   - arm_transfer()      only RECORDS the phase (no DMA descriptor),
 *   - poll_service()      executes it and owns READY end-to-end,
 *   - reinit / release / suspend / quiesce  are NO-OPS (reinit's sole exception
 *     is spi == NULL: no handle, so nothing can be blocked inside SPI_transfer,
 *     and the tick's dead-slave re-open is update mode's only recovery).
 *
 * Those four no-ops are the root-cause fix, placed here rather than in each of
 * the ~24 callers across cc3501e_hw_ti{,_ble,_wifi,_ota}.c and worker.c.  In
 * update mode nothing was ever torn down, so re-rolling SPI_open's retry dice
 * can only lose; closing the handle while a task is blocked inside a polled
 * SPI_transfer is the exact deadlock that killed the dedicated poll task; and
 * quiesce makes arm_transfer() silently DROP a recorded phase, desyncing the
 * host mid-frame.
 *
 * ACCEPTED CONSEQUENCE: in update mode cc3501e_hw_tick()'s desync-burst and
 * arm-failure self-heals still COUNT but never heal (only the dead-slave re-open
 * still works, see above).  What used to make that fatal -- and no longer does --
 * is spi_fifo_reset(): spiPollingTransfer ENABLES the SPI IP and never disables
 * it again, and the driver flushes the FIFOs only inside SPI_open, so every byte
 * the host clocked BETWEEN two polled transfers stayed latched and was counted
 * against the NEXT phase, i.e. a PERMANENT byte-phase shift with no self-heal.
 * poll_service() now resets both FIFOs at each frame boundary, so a deaf gap
 * costs the host a retry instead of the link.  The host's remaining escape from
 * a desync is a WIFI_EN/nRESET cold cycle, which always lands in NORMAL mode
 * because the flag lives in RAM only.
 * =====================================================================
 *
 * CONFIG_SPI_0 is the SysConfig anchor for the inter-chip SPI instance,
 * resolved at bench-build time from the E1M-AEN board file.
 * =====================================================================
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ti_drivers_config.h"

#include <ti/drivers/SPI.h>
#include <ti/drivers/spi/SPIWFF3DMA.h> /* SPIWFF3DMA_CMD_RETURN_PARTIAL_ENABLE -- CS-framed re-sync */
#include <ti/drivers/dpl/ClockP.h>     /* ClockP_usleep -- settle between SPI re-open retries */

/* Direct SPI register access, for the polled bridge's per-frame FIFO reset
 * (spi_fifo_reset() below).  The driver's own flushFifos() is static, and the
 * only public API that reaches it is SPI_transferCancel() -- which has hung this
 * bridge twice and is banned on every path here.  DeviceFamily_CC35XX comes from
 * ti/build_ti.ps1's -D flags, and this backend is CC35xx-only, so the concrete
 * paths are spelled out rather than routed through
 * DeviceFamily_constructPath() -- clang-format reads `inc/hw_spi.h` inside that
 * macro as a division and reformats it into a broken header name. */
#include <ti/devices/DeviceFamily.h>
#include <ti/devices/cc35xx/inc/hw_spi.h>
#include <ti/devices/cc35xx/inc/hw_types.h>

#include "../../src/protocol.h"
#include "../../src/transport.h"

#include "../cc3501e_hw.h" /* cc3501e_hw_notify_reply_sent -- arm the deferred reset/OTA-swap reboot */

/* SS0-framed protocol phases (see file header). */
enum spi_phase {
	PH_REQ_HEADER = 0, /* clocking the 4-byte request header   */
	PH_REQ_PAYLOAD,    /* clocking payload_len request bytes   */
	PH_REPLY_HEADER,   /* clocking the 4-byte reply header     */
	PH_REPLY_PAYLOAD,  /* clocking the reply payload           */
};

static SPI_Handle     spi;
static enum spi_phase phase;

/* One in-flight request frame + its staged reply (header + max payload). */
static uint8_t  frame_buf[CC3501E_FRAME_MAX_BYTES];
static uint8_t  reply_buf[CC3501E_FRAME_MAX_BYTES];
static size_t   reply_len;
static uint16_t cur_payload_len;

/* Header-idle SYNC marker (ALP_CC3501E_SYNC_IDLE = 0xA5) driven on MISO while
 * the slave is parked at a frame boundary (clocking a request header).
 * CONTRACT-DEFINED, not bench diagnostics: at a clean frame boundary the host
 * keys its byte-alignment sync + desync recovery off a run of 0xA5 (see
 * chips/cc3501e/cc3501e.c cc3501e_sync()).  During the request PAYLOAD phase the
 * slave drives NULL (0x00) instead -- 0xA5 marks ONLY the header-phase boundary,
 * so the host can distinguish "parked at a clean boundary" from "mid-payload". */
static uint8_t sync_idle[ALP_CC3501E_HEADER_BYTES];

/* Zero-filled dummy TX buffer for the request-PAYLOAD phase (see arm_transfer's
 * PH_REQ_HEADER caller below).  That phase is RX-only (the host is clocking ITS
 * payload bytes into us) so the driver logically doesn't care what goes out on
 * MISO, and a literal NULL txBuf documents "don't care" -- but SPIWFF3DMA's
 * transfer-arm path treats a NULL txBuf as "no TX descriptor to program", which
 * on this backend can make SPI_transfer() reject the arm outright rather than
 * silently substitute a fill byte.  A real (non-NULL), zeroed buffer keeps the
 * exact same 0x00-on-MISO wire behaviour while always giving SPIWFF3DMA a valid
 * descriptor to program.  Static + BSS-zeroed, sized to the largest payload the
 * protocol allows so every plen in [1, ALP_CC3501E_MAX_PAYLOAD] fits. */
static uint8_t dummy_tx_zero[ALP_CC3501E_MAX_PAYLOAD];

/* P0-2 desync/probe re-arm counter (KEPT -- route through DIAG_GET_STATS for
 * link-health observability).  Counts header-phase re-arms triggered by a
 * reserved-range / all-0xFF header (a host sync-probe, or byte-misalignment). */
volatile uint32_t g_resync_count;

/* Count of bridge SPI (re-)opens: 1 = initial open, then +1 per radio-op
 * recovery (bridge_transport_spi_hw_reinit).  Observable for link-health. */
volatile uint32_t g_spi_reopen_count;

/* Count of SPI_transfer() arm failures (#1133): SPI_transfer() itself refused
 * to queue the DMA descriptor -- the slave is left un-armed and READY low
 * (see arm_transfer below).  Nothing else re-arms that phase, so left alone
 * this is a PERMANENT stall, not a transient one.  cc3501e_hw_tick() polls
 * this counter (the same burst-detection shape as g_resync_count) and drives
 * a full SPI re-open to recover -- the same proven reinit path a radio op
 * already relies on. */
volatile uint32_t g_arm_fail_count;

/* ---------------- persisted OTA-update-mode boot flag ------------------ */

/* Magic armed by ALP_CC3501E_CMD_OTA_UPDATE_MODE (0x47) and consumed on the
 * next boot.  Arbitrary but recognisable; stored WITH its one's complement so a
 * half-written or scrubbed struct can never read as "armed". */
#define CC3501E_POLLED_BOOT_MAGIC 0x0A7AB007u

/* Persisted across the device's OWN warm reset (NVIC_SystemReset = SYSRESETREQ),
 * NOT across a host WIFI_EN/nRESET cold cycle -- that asymmetry IS the escape
 * hatch: a wedged update-mode boot is always one cold cycle away from normal
 * mode, and update mode has no other recovery (see the file header).
 *
 * WARNING: RAM retention across SYSRESETREQ is an ASSUMPTION on this silicon,
 * not a code-proven fact.  ResetISR (ti/startup_ticlang_local.c) does not scrub
 * RAM and --rom_model auto-init only touches sections listed in the .cinit
 * table, but the CC35xx SES/boot ROM runs BEFORE ResetISR and its source is not
 * in this repo.  Hence the magic + complement pair -- anything short of an exact
 * match reads as NORMAL mode -- and hence bridge_transport_spi_boot_mark(),
 * which is what actually PROVES retention on the bench (the chip has no UART on
 * the debug probe, so GET_DIAG_INFO reserved[2] is the only channel).
 *
 * LINKER PLACEMENT IS A HARD PREREQUISITE.  The stock TI connectivity
 * linker.cmd this build copies verbatim declares NO .TI.noinit output section,
 * so without an added placement rule (`.TI.noinit : {} > DRAM_NON_SECURE`,
 * origin 0x28000DB0) the TI linker falls back to default rules and lands this
 * struct in the first fitting range -- FLASH_INT_VEC (RWX) at 0x14000000, i.e.
 * flash.  persist_writable() below makes that outcome FAIL SAFE rather than
 * fatal, but update mode is then silently dead: check the linker map puts
 * g_persist inside 0x28000DB0..0x2807FFFF. */
static struct {
	uint32_t magic;
	uint32_t magic_inv;
	uint32_t boots;
} g_persist __attribute__((section(".TI.noinit")));

static bool g_polled;    /* this boot's mode, latched on first query */
static bool g_boot_read; /* the read-and-clear has run */

/* Guard the placement prerequisite above.  Every RAM range in the vendor
 * linker.cmd starts at or above 0x20000000 (TCM_DRAM 0x20000000, DRAM
 * 0x28000DB0); every flash/code range is below it (TCM_CRAM 0x00000000, CRAM
 * 0x08000000, FLASH_INT_VEC 0x14000000).  Storing into a mis-placed struct
 * would BusFault -> hard fault -> boot loop, which is a far worse failure than
 * update mode simply never arming. */
static bool persist_writable(void)
{
	return (uintptr_t)&g_persist >= 0x20000000u;
}

bool bridge_transport_spi_polled(void)
{
	if (!g_boot_read) {
		g_boot_read = true;
		if (!persist_writable()) {
			return false; /* g_polled stays false; never touch the struct */
		}
		const bool valid = (g_persist.magic == ~g_persist.magic_inv);
		if (!valid) {
			/* RAM was scrubbed, or this is the first boot after a cold cycle --
			 * restart the counter so a value stuck at 1 on the bench means
			 * "scrubbed on every boot", unambiguously. */
			g_persist.boots = 0u;
		}
		g_persist.boots++;
		g_polled = valid && (g_persist.magic == CC3501E_POLLED_BOOT_MAGIC);
		/* READ-AND-CLEAR.  A wedged update-mode boot must never repeat: without
		 * this the device would boot into a deaf, self-heal-less loop forever. */
		g_persist.magic     = 0u;
		g_persist.magic_inv = ~0u;
	}
	return g_polled;
}

bool bridge_transport_spi_request_polled_boot(void)
{
	/* Reports whether the flag was actually ARMED.  It used to return void, so a
	 * non-writable persist area (a mis-placed .TI.noinit, or a boot ROM that
	 * scrubs the retained RAM) was a silent no-op -- while the caller went on to
	 * reset anyway.  The device then came back in NORMAL mode, the host's confirm
	 * loop re-issued 0x47, and every re-issue armed another warm reset: roughly
	 * 14 resets across a 20 s budget, ending in a hard reset.  Retained RAM
	 * surviving SYSRESETREQ is an ASSUMPTION on this silicon (see the header of
	 * this file), not a proven fact, so the caller must be able to refuse. */
	if (!persist_writable()) {
		return false;
	}
	g_persist.magic     = CC3501E_POLLED_BOOT_MAGIC;
	g_persist.magic_inv = ~CC3501E_POLLED_BOOT_MAGIC;
	return true;
}

bool bridge_transport_spi_clear_polled_boot(void)
{
	if (!persist_writable()) {
		return false;
	}
	g_persist.magic     = 0u;
	g_persist.magic_inv = ~0u;
	return true;
}

uint8_t bridge_transport_spi_boot_mark(void)
{
	if (!persist_writable()) {
		/* Mis-placed struct: report a clean 0 rather than whatever bytes happen to
		 * sit at that flash address, so the bench reads three DISTINCT signatures --
		 * 0 = placement broken (or stub backend), counter stuck at 1 = RAM scrubbed
		 * across the warm reset, counter incrementing = retention proven. */
		return 0u;
	}
	return (uint8_t)((g_polled ? 0x80u : 0x00u) | (uint8_t)(g_persist.boots & 0x7Fu));
}

/* ----------------------------------------------------------------------- */

/* Quiesce gate.  While set, on_transfer() must NOT re-arm: the OTA pump needs the
 * slave to reach the SAME state boot has -- open handle, NO transfer armed, so no
 * DMA channel held -- because psa_fwu_start() returns promptly at boot and never
 * returns from the pump with a transfer armed.  SPI_close alone does not free an
 * armed channel and SPI_transferCancel does not return here, so the only way to
 * get there is to stop re-arming and let the in-flight transfer retire on its own
 * (the host is already held off by cc3501e_bridge_busy()). */
static volatile bool g_quiesce;

void bridge_transport_spi_hw_quiesce(bool on)
{
	/* OTA update mode: quiesce is not merely useless here, it is HARMFUL.  The
	 * update-mode loop is strictly sequential (nothing runs concurrently with the
	 * flash op), and while quiesced arm_transfer() returns early -- so a phase
	 * recorded at the end of a frame is silently DROPPED and the host desyncs
	 * mid-frame, with no self-heal left to catch it. */
	if (bridge_transport_spi_polled()) {
		return;
	}
	g_quiesce = on;
}

/* Polled bridge (OTA update mode): the pending phase descriptor.  arm_transfer()
 * only RECORDS it; bridge_transport_spi_poll_service() executes it with a
 * BLOCKING SPI_transfer and then re-enters the very same phase machine.  Unused
 * in normal (callback/DMA) mode.
 *
 * NB the mechanism is NOT "a polled open claims no DMA" -- SPIWFF3DMA_open calls
 * initDMA() unconditionally, so a polled open still CONFIGURES the DMA.  What
 * differs is that no DMA transaction is ever PRIMED (the driver's polling branch
 * skips primeTransfer).  Empirically that is the difference psa_fwu cares about:
 * psa_fwu_start/psa_fwu_write both return after a polled SPI_open and neither
 * ever returns after a callback/DMA one (silicon 2026-08-21). */
static void       *pend_rx;
static const void *pend_tx;
static size_t      pend_count;
static bool        pend_valid;

/* Arm one SS0-framed phase transfer.  For RX, tx is the 0xA5 marker (header) or
 * NULL (payload -> 0x00 default fill on MISO); for TX, rx is NULL (the host's
 * MOSI dummies are discarded).  Non-blocking in SPI_MODE_CALLBACK: the DMA
 * (RX=ch12, TX=ch13) drains/fills the FIFO and the driver invokes on_transfer
 * when `count` frames have shifted. */
static void arm_transfer(void *rx, const void *tx, size_t count)
{
	static SPI_Transaction t; /* retained for the transfer's duration */
	if (g_quiesce) {
		/* Deliberately leave READY LOW: not armed means the host must not clock. */
		return;
	}
	if (bridge_transport_spi_polled()) {
		/* Polled/update mode: RECORD the phase; poll_service() executes it.
		 *
		 * Do NOT raise READY here.  Recording a phase is not arming it: in polled
		 * mode the slave only starts listening when the service loop enters
		 * SPI_transfer.  Raising READY at record time told the host "armed" while
		 * the slave was still idle, so it clocked into nothing and every request
		 * failed (-5) even though the bytes landed later -- measured at ~26 B/s.
		 * The service loop raises it immediately before the blocking transfer. */
		pend_rx    = rx;
		pend_tx    = tx;
		pend_count = count;
		pend_valid = true;
		return;
	}
	t.count = count;
	t.txBuf = (void *)tx;
	t.rxBuf = rx;
	t.arg   = NULL;
	if (!SPI_transfer(spi, &t)) {
		/* SPI_transfer() itself failed to queue the DMA descriptor (e.g. a
		 * transfer already in flight, or SPIWFF3DMA rejected the arm) -- the
		 * slave is NOT actually armed for the next clock, even though the
		 * caller thinks it is.  Previously this return value was discarded
		 * and READY was raised unconditionally, which is a LIE: the host
		 * would see READY, assert CSN, and clock into a slave that never
		 * latches -- a silent byte-misalignment recoverable only via the
		 * host's desync walk (cc3501e_sync()).  Leaving READY LOW here
		 * instead gives the host a real, visible stall (its READY-gate wait
		 * times out) that its poll-by-repeat retries against, rather than a
		 * silently corrupted frame.
		 *
		 * READY-low alone does not self-heal: nothing else re-attempts this
		 * arm, so without more the stall is PERMANENT (#1133).  Bump the
		 * fail counter so cc3501e_hw_tick() drives a full SPI re-open
		 * recovery (bridge_transport_spi_hw_reinit) -- see g_arm_fail_count. */
		g_arm_fail_count++;
		return;
	}
	/* Slave is now armed -> raise READY so the host may clock THIS transfer.  Paired
	 * with the CSN-deassert re-arm + on_transfer's busy() drop, this gives the host an
	 * exact "armed" edge to gate on, so it never asserts CSN + clocks into a not-yet-
	 * re-armed slave (which loses the header's first bits -> misread -> 0x00 desync). */
	cc3501e_bridge_ready();
}

/* Re-arm the request-header phase driving the 0xA5 marker on MISO.  Used both
 * for the normal next-frame re-arm and the P0-2 desync/probe no-op re-arm. */
static void arm_request_header(void)
{
	phase = PH_REQ_HEADER;
	arm_transfer(frame_buf, sync_idle, ALP_CC3501E_HEADER_BYTES);
}

/* Replay the captured request frame through the silicon-free seams
 * (which build the staged reply), then drain that reply into reply_buf. */
static void dispatch_frame(size_t frame_len)
{
	spi_slave_cs_low();
	for (size_t i = 0; i < frame_len; i++) {
		spi_slave_rx_byte(frame_buf[i]);
	}
	spi_slave_cs_high();

	/* ONE memcpy, not two cross-TU calls per byte.  The old loop drained the
	 * staged reply a byte at a time through spi_slave_tx_pending() /
	 * spi_slave_tx_next_byte(), which live in another translation unit and are
	 * not inlined (no LTO), so every payload byte cost two real function calls --
	 * a SECOND per-byte copy on top of the single bulk build that
	 * protocol_build_reply() already performed into spi_tx_buf.
	 *
	 * The host sees this as its reply-HEADER gate: silicon-measured 2026-08-24 at
	 * ~0.68 us/byte (g3=1147 us for a 1683-byte reply, g3=1301 us for 1925), i.e.
	 * ~1.2 ms of a 2.9 ms SOCK_RECV transaction, and it is why a 2.5x faster HOST
	 * core changed throughput by 0%. */
	reply_len = (uint16_t)spi_slave_tx_take(reply_buf, sizeof(reply_buf));
	/* The reply is always header(4) + payload(>=1 status byte). */
}

/* SPI transfer-complete callback (driver SWI/HWI context).  Advances the
 * request-header -> request-payload -> reply-header -> reply-payload phases. */
/* ---- Reply-phase stall watchdog ------------------------------------------
 *
 * A transaction the host abandons mid-way -- an error or timeout between phases
 * AFTER the slave already armed its reply -- leaves that transfer armed forever.
 * SPI_TRANSFER_RETURN_PARTIAL is deliberately OFF (it drops READY after the arm
 * and stalls the host READY gate), so the CS deassert does NOT complete it, and
 * the host's short retries only add bytes to the outstanding count.  Neither
 * existing self-heal sees it: g_resync_count does not move (the slave is not
 * misframing) and g_arm_fail_count does not move (the arm succeeded).  Bench
 * signature: the host in-band marker reads [02 00 00 00] once then
 * [00 00 00 00] forever, and a 256 kB read dies 61-212 kB in.
 *
 * Only REPLY phases are watched: PH_REQ_HEADER legitimately waits forever for
 * the next request, so watching it would fire on every idle gap.  A reply is
 * only ever armed immediately after a request arrived.
 *
 * The armed flag is a SEPARATE bool, not a sentinel packed into the timestamp.
 * A previous attempt stamped `uptime | 1u` to mean "armed", which for any EVEN
 * uptime makes the stamp LARGER than now -- the elapsed subtraction then
 * underflows to ~4e9 and the watchdog fires on every single frame.  That reinit
 * storm, not the threshold, is what collapsed throughput to 328-3098 B/s. */
#define CC3501E_REPLY_STALL_MS 250u

static volatile bool     reply_armed;
static volatile uint32_t reply_armed_ms;

bool bridge_transport_spi_reply_stalled(void)
{
	if (!reply_armed) {
		return false;
	}
	return (uint32_t)(cc3501e_hw_uptime_ms() - reply_armed_ms) > CC3501E_REPLY_STALL_MS;
}

static void on_transfer(SPI_Handle h, SPI_Transaction *t)
{
	(void)h;
	reply_armed = false; /* a phase completed -> nothing outstanding */
	/* A phase's transfer just ended -> the slave is momentarily NOT armed for the host's
	 * next clock.  Drop READY so the host holds off until arm_transfer() re-raises it. */
	cc3501e_bridge_busy();

	/* Per-transfer HARDWARE SS0: the Alif master asserts/deasserts SS0 around EACH
	 * transceive, so every phase (req header / [req payload] / reply header / reply
	 * payload) is its own CS-framed transfer.  ADVANCE on the transfer-COMPLETE callback
	 * (the host clocked the full phase count): that is the event the driver delivers per
	 * phase.  A redundant late SS0-deassert (SPI_TRANSFER_CSN_DEASSERT, if RETURN_PARTIAL
	 * also delivers one for the already-completed transfer) is ignored to avoid
	 * double-advancing.  Advancing here also re-raises READY (via arm_transfer), so the
	 * host's per-request READY gate sees the slave armed for the next phase. */
	if (t == NULL || t->status != SPI_TRANSFER_COMPLETED) {
		/* SPI_TRANSFER_CANCELED is the ONE non-COMPLETED status this backend
		 * expects: it is exactly what bridge_transport_spi_hw_suspend() drives
		 * via SPI_transferCancel() right before SPI_close(), i.e. the slave is
		 * being torn down ON PURPOSE -- leave it alone (spi_open_and_arm()
		 * re-arms fresh on the paired reinit).  Any OTHER non-COMPLETED status
		 * (SPI_TRANSFER_FAILED, or an unexpected mid-phase status this
		 * hardware-SS0, RETURN_PARTIAL-disabled framing should not otherwise
		 * deliver) is an UNPLANNED transfer failure.  Previously this fell
		 * through the same early return with no recovery: no callback is ever
		 * pending on a dead transfer, so the slave stayed un-armed (and READY
		 * low) FOREVER -- a permanent wedge only a hard reset could clear.
		 * Re-arm the request-header phase instead, so the host's next PING /
		 * poll-retry can resynchronise on this rev, not just on a reboot. */
		if (t != NULL && t->status != SPI_TRANSFER_CANCELED) {
			arm_request_header();
		}
		return;
	}

	switch (phase) {
	case PH_REQ_HEADER: {
		/* Desync/probe guard (P0-2): a header whose cmd byte is in the reserved
		 * range (>= 0x80, which includes an all-0xFF idle/probe header) is NOT a
		 * valid v1 request.  It means the host is probing/re-syncing or byte
		 * alignment drifted -- do NOT dispatch; just re-arm the header phase
		 * (keep driving 0xA5) so the host's byte-walk lands on a clean boundary.
		 * Makes the sync handshake non-destructive. */
		if (frame_buf[0] >= ALP_CC3501E_CMD_RESERVED_VENDOR_BASE) {
			g_resync_count++;
			arm_request_header();
			break;
		}

		/* Bound the declared payload to the wire ceiling so a garbage length
		 * can't overrun the RX into frame_buf; an over-long declared length then
		 * fails the seam's captured-vs-declared check as RESP_ERR_PROTOCOL. */
		uint16_t plen = (uint16_t)frame_buf[2] | ((uint16_t)frame_buf[3] << 8);
		if (plen > ALP_CC3501E_MAX_PAYLOAD) {
			plen = ALP_CC3501E_MAX_PAYLOAD;
		}
		cur_payload_len = plen;
		if (plen == 0u) {
			dispatch_frame(ALP_CC3501E_HEADER_BYTES);
			/* Reply HEADER as its OWN SS0-framed transfer: under per-transfer hardware
			 * CS the host reads the 4-byte reply header in one transceive, then sizes +
			 * reads the payload in the NEXT.  (No single-transfer reply -- the SS0
			 * deassert after the header would cut a single armed reply mid-frame.) */
			phase = PH_REPLY_HEADER;
			arm_transfer(NULL, reply_buf, ALP_CC3501E_HEADER_BYTES);
			reply_armed_ms = cc3501e_hw_uptime_ms();
			reply_armed    = true;
		} else {
			phase = PH_REQ_PAYLOAD;
			/* dummy_tx_zero (all-0x00) on MISO during payload (0xA5 marks the
			 * header boundary only) -- see dummy_tx_zero's comment: SPIWFF3DMA
			 * needs a real txBuf to arm, a literal NULL is not safe here. */
			arm_transfer(&frame_buf[ALP_CC3501E_HEADER_BYTES], dummy_tx_zero, plen);
		}
		break;
	}
	case PH_REQ_PAYLOAD:
		dispatch_frame((size_t)ALP_CC3501E_HEADER_BYTES + cur_payload_len);
		/* Reply HEADER as its own SS0-framed transfer (see PH_REQ_HEADER). */
		phase = PH_REPLY_HEADER;
		arm_transfer(NULL, reply_buf, ALP_CC3501E_HEADER_BYTES);
		reply_armed_ms = cc3501e_hw_uptime_ms();
		reply_armed    = true;
		break;

	case PH_REPLY_HEADER:
		/* Reply PAYLOAD (status + data = reply_len - 4, always >= 1) as its OWN
		 * SS0-framed transfer, after the host clocked the reply header in the previous
		 * transceive (so it knows the length to clock here). */
		phase = PH_REPLY_PAYLOAD;
		arm_transfer(
		    NULL, &reply_buf[ALP_CC3501E_HEADER_BYTES], reply_len - ALP_CC3501E_HEADER_BYTES);
		reply_armed_ms = cc3501e_hw_uptime_ms();
		reply_armed    = true;
		break;

	case PH_REPLY_PAYLOAD:
	default:
		/* Whole reply clocked -- clean frame boundary.  Tell the HAL the in-flight
		 * reply has fully drained so the deferred CMD_RESET and OTA swap-reboot
		 * (both gated on reply_drained) can fire on the next cc3501e_hw_tick() --
		 * without this the host's ack never marks the link quiescent and neither
		 * deferred reboot ever runs.  Then re-arm the next request header (0xA5). */
		cc3501e_hw_notify_reply_sent();
		arm_request_header();
		break;
	}
}

/* Reset the SPI RX **and** TX FIFOs.  POLLED MODE ONLY -- never call this with a
 * transfer in flight (see the caller).
 *
 * THE ROOT CAUSE THIS EXISTS FOR (silicon 2026-08-21; do not re-derive):
 * spiPollingTransfer() calls enableSPI() and NEVER disables the IP again, and the
 * driver flushes the FIFOs only inside SPI_open().  So the slave keeps LATCHING
 * host bytes while no polled transfer is running, and spiPollingTransfer counts
 * dataGet() reads, not SCLK edges -- with RETURN_PARTIAL disabled (mandatory: it
 * is one of the four terms that select the polling branch, SPIWFF3DMA.c:774) the
 * SS0 deassert is invisible, so a stale FIFO byte satisfies the NEXT phase
 * instantly and the device runs permanently ahead of the host.  Every gap between
 * two polled transfers therefore costs a PERMANENT byte-phase shift, and the RX
 * FIFO holds only 8 frames before it silently overruns (SPIWFF3DMA.h:84-86, :97;
 * the RX-overrun IRQ that would flush is enabled ONLY in the DMA branch,
 * SPIWFF3DMA.c:800-806, so nothing self-heals).
 *
 * Parking a task inside SPI_transfer does NOT fix this -- the flash pump preempts
 * that task for the whole psa_fwu op (a slot erase measures 22-41 s here) and
 * nothing on the psa_fwu path yields -- so the fix is to make the gap HARMLESS
 * instead of trying to abolish it: start every frame from empty FIFOs.  A gap then
 * costs the host one failed frame it already retries, not the link.
 *
 * The TX half matters too: a reply the host stopped clocking mid-phase leaves
 * bytes in the TX FIFO that would otherwise prefix the next phase on MISO.
 *
 * Mirrors the driver's static flushFifos() (SPIWFF3DMA.c:1182): the IP must be
 * DISABLED for FIFORST to take.  SPI_transfer -> spiPollingTransfer -> enableSPI()
 * re-enables it (isSPIEnabled reads the register, SPIWFF3DMA.c:1586, so the driver
 * cannot cache a stale "already on"). */
static void spi_fifo_reset(void)
{
	if (spi == NULL) {
		return;
	}
	const uint32_t base = ((const SPIWFF3DMA_HWAttrs *)spi->hwAttrs)->baseAddr;
	HWREG(base + SPI_O_CTL1) &= ~SPI_CTL1_EN_ENABLE;
	HWREG(base + SPI_O_CTL0) |= SPI_CTL0_FIFORST_RST_TRIG;
	while ((HWREG(base + SPI_O_CTL0) & SPI_CTL0_FIFORST) == SPI_CTL0_FIFORST_RST_TRIG) {
	}
	/* Clear the sticky RX-overrun latch the blackout almost certainly set.  The
	 * driver only ever clears it from its RX-overrun ISR, which the polling branch
	 * never arms (SPIWFF3DMA.c:800-806 enables SPI_MIS_RXOVF_SET in the DMA branch
	 * only), so without this it stays set for the rest of the boot and any future
	 * unmask would fire on stale history. */
	HWREG(base + SPI_O_ICLR) = SPI_MIS_RXOVF_SET;
}

/* Run ONE pending phase to completion.  SPI_transfer blocks until the host
 * clocks it (peripherals only get the polling path with SPI_WAIT_FOREVER), so
 * this must own its calling loop -- fine for an OTA build where nothing else
 * runs.  Returns true if a phase completed, so the caller knows a frame boundary
 * may have been reached and it is safe to run flash work with nothing in
 * flight. */
bool bridge_transport_spi_poll_service(void)
{
	if (spi == NULL || !pend_valid) {
		return false;
	}
	/* FRAME BOUNDARY = the only safe reset point, and the only one that is needed.
	 * Safe: no transfer is in flight here (arm_transfer only RECORDED the phase), so
	 * no spiPollingTransfer rxCount is half-consumed.  Needed: the caller runs the
	 * OTA pump between two calls of this function, and everything the host clocked
	 * across that blackout is stale by definition -- discard it rather than let it
	 * satisfy this frame's phases.  See spi_fifo_reset() for why that is fatal.
	 *
	 * NOT inside the per-phase loop below: the host's 200 us CC3501E_PHASE_SETTLE_US
	 * covers the handful of instructions between two phases of the SAME frame, and a
	 * reset there could eat a legitimately early byte instead. */
	spi_fifo_reset();
	/* Service the WHOLE frame, not one phase per call.  A request is four
	 * SS0-framed phases (req header / [req payload] / reply header / reply
	 * payload); returning to the caller between them let the host's per-request
	 * timeout expire before the reply was clocked -- measured at ~23 B/s, with
	 * the data still landing late (dev_cursor advanced while the host logged
	 * write=-5).  Run phases back-to-back until the machine returns to the
	 * request-header phase, i.e. a clean frame boundary, so flash work in the
	 * caller still happens with nothing in flight. */
	static SPI_Transaction t;
	bool                   serviced = false;
	for (unsigned guard = 0u; guard < 8u; ++guard) {
		if (!pend_valid) {
			break;
		}
		t.count    = pend_count;
		t.txBuf    = (void *)pend_tx;
		t.rxBuf    = pend_rx;
		t.arg      = NULL;
		pend_valid = false;
		/* READY is owned SOLELY by this function in polled mode -- not by main.c at
		 * boot and not by the OTA pump.  Known residual window: READY goes high
		 * here, a few instructions BEFORE spiPollingTransfer re-enables the SPI IP,
		 * so a very fast host can clock the first byte early.  Suspect this first if
		 * the head byte of a phase reads wrong.  It cannot be closed from here (the
		 * driver owns the enable) and the obvious alternative -- raise READY after
		 * SPI_transfer returns -- is impossible: that call does not return until the
		 * host has already clocked the whole phase. */
		cc3501e_bridge_ready();
		if (!SPI_transfer(spi, &t)) {
			g_arm_fail_count++;
			arm_request_header();
			/* DROP READY before leaving.  It was raised just above, and
			 * arm_request_header() only RECORDS the phase in polled mode -- it
			 * deliberately does not raise READY -- so breaking here left the line
			 * HIGH with nothing armed.  That is exactly the lie this file's own
			 * rule forbids: the slave advertises "clock away" while no transfer is
			 * in flight, for at least the 1 ms tick in firmware/cc3501e/src/main.c,
			 * and the host's bytes land in a slave that is not listening.  It
			 * self-heals only because spi_fifo_reset() discards them on the next
			 * frame, i.e. it costs one desynced frame every time -- and this branch
			 * fires precisely when the shared DMA is busy just after a flush. */
			cc3501e_bridge_busy();
			break;
		}
		serviced = true;
		cc3501e_bridge_busy();
		t.status = SPI_TRANSFER_COMPLETED;
		on_transfer(spi, &t); /* phase machine; its arm_transfer() only records */
		if (phase == PH_REQ_HEADER) {
			break; /* frame complete -- safe point for flash work */
		}
	}
	return serviced;
}

/* Open the bridge SPI0 slave -- SPI_MODE_CALLBACK with DMA on the free ch12/13
 * (per the board file) in normal mode, SPI_MODE_BLOCKING / SPI_WAIT_FOREVER in OTA
 * update mode; bridge_transport_spi_polled() picks, see below -- fill the 0xA5
 * header-idle marker, and arm the first request header.  Shared by init and
 * reinit.  MUST run on the bring-up/worker task. */
/* Set when every SPI_open retry failed, so the slave has no handle at all.
 * cc3501e_hw_tick() polls this and re-attempts the open -- see the note in
 * spi_open_and_arm().  Read cross-TU via bridge_transport_spi_is_dead(). */
volatile uint32_t g_spi_open_failed;

static void spi_open_and_arm(void)
{
	for (size_t i = 0u; i < sizeof(sync_idle); i++) {
		sync_idle[i] = ALP_CC3501E_SYNC_IDLE;
	}

	SPI_Params params;
	SPI_Params_init(&params);
	params.mode        = SPI_PERIPHERAL; /* CC35xx TI Drivers term for SPI slave */
	params.frameFormat = SPI_POL0_PHA0;  /* mode 0, per the host driver / chip manifest */
	params.dataSize    = 8;

	if (bridge_transport_spi_polled()) {
		/* OTA update mode.  SPIWFF3DMA takes its polling path only when
		 * transferMode == SPI_MODE_BLOCKING, transaction->count <
		 * hwAttrs->minDmaTransferSize, RETURN_PARTIAL is disabled (see below) and
		 * -- as a PERIPHERAL -- transferTimeout is SPI_WAIT_FOREVER.  All four
		 * terms must hold; WAIT_FOREVER means a polled transfer blocks until the
		 * host clocks it, which is why only the dedicated update-mode loop may
		 * drive this mode.  minDmaTransferSize is hard-emitted as 10 by SysConfig
		 * with no property to override it, so ti/build_ti.ps1 patches the generated
		 * ti_drivers_config.c to 1024 -- that ceiling must clear the largest frame
		 * the protocol allows, ALP_CC3501E_HEADER_BYTES + ALP_CC3501E_MAX_PAYLOAD =
		 * 4 + 512 = 516 B.  Never trim it toward the ~260 B that today's 256-byte
		 * host OTA chunk happens to produce. */
		params.transferMode    = SPI_MODE_BLOCKING;
		params.transferTimeout = SPI_WAIT_FOREVER;
	} else {
		params.transferMode        = SPI_MODE_CALLBACK; /* DMA on the free ch12/13 */
		params.transferCallbackFxn = on_transfer;
	}

	/* Retry SPI_open: right after a psa_fwu flash burst the shared DMA can be
	 * momentarily busy, so a single open intermittently returns NULL -> the slave
	 * stays dead and the host's poll times out (the OTA-finish flakiness, silicon
	 * 2026-06-19).
	 *
	 * The budget used to be 8 x 2 ms = 16 ms flat, which is enough for the ONE
	 * burst OTA FINISH performs.  Windowed OTA (#1610) re-arms after EVERY window
	 * flush -- ~67 times for a 1.09 MB image -- so 16 ms is rolled ~67 times, and
	 * the first roll that loses killed the link permanently: measured on silicon,
	 * the stream survives hundreds of seconds of flushes and then dies for good.
	 * Back off progressively to ~100 ms total instead of a flat 16 ms. */
	for (int attempt = 0; attempt < 12; attempt++) {
		spi = SPI_open(CONFIG_SPI_0, &params);
		if (spi != NULL) {
			break;
		}
		ClockP_usleep((uint32_t)(2000 + attempt * 1500)); /* 2 ms .. 18.5 ms */
	}
	g_spi_reopen_count++;
	if (spi == NULL) {
		/* DEAD, and NOTHING else will notice.  This is the one failure the tick's
		 * two self-heals cannot see: with no handle there are no transfers (so
		 * g_resync_count never bumps) and no arm attempts (so g_arm_fail_count
		 * never bumps), and the old code simply returned -- a permanent, silent
		 * wedge recoverable only by a debug-probe reflash.
		 *
		 * Latch it so cc3501e_hw_tick() keeps retrying the open.  A busy DMA is
		 * transient by nature; the only thing that made it terminal was never
		 * trying again. */
		g_spi_open_failed = 1u;
		return;
	}
	g_spi_open_failed = 0u;

	/* RETURN_PARTIAL is intentionally NOT enabled.  With hardware SS0 (the Alif master
	 * drives the per-transfer chip-select) each phase's transfer completes on its byte
	 * count -- on_transfer advances on SPI_TRANSFER_COMPLETED before the SS0 deasserts.
	 * Enabling RETURN_PARTIAL would ALSO deliver a trailing SPI_TRANSFER_CSN_DEASSERT per
	 * phase, which (after on_transfer already re-armed + raised READY) would drop READY
	 * again and stall the host's per-request READY gate (-3 BUSY).  Bench-proven. */

	g_resync_count = 0u;
	arm_request_header();
}

/* Release the slave and its DMA WITHOUT SPI_transferCancel.  The cancel has now
 * hung the bridge twice on silicon (inside reinit, and inside suspend called from
 * the OTA pump), so it is unusable on this path even with the host held off.
 * SPI_close alone tears the peripheral down, which is what psa_fwu_start needs:
 * called at BOOT with no slave armed it returns promptly (PSA_ERROR_BAD_STATE
 * 0x77), called from the pump with a live SPI_MODE_CALLBACK transfer it never
 * returns at all.  Pair this with bridge_transport_spi_hw_reinit() afterwards. */
void bridge_transport_spi_hw_release(void)
{
	/* OTA update mode: NEVER close the handle.  This bare SPI_close is precisely
	 * what closed the handle out from under a task blocked inside a polled
	 * SPI_transfer and deadlocked the dedicated poll task.  Nothing needs
	 * releasing anyway -- no DMA transaction is ever primed in polled mode. */
	if (bridge_transport_spi_polled()) {
		return;
	}
	if (spi != NULL) {
		SPI_close(spi);
		spi = NULL;
	}
	phase = PH_REQ_HEADER;
}

/* Re-open + re-arm the bridge slave after a radio op (boot Wlan_Start or a
 * worker Wlan_* body).  Called from the BRING-UP / worker task -- NEVER the SPI
 * callback -- and ONLY right after a radio op, when the radio's global DMA
 * re-init has already torn the slave's DMA down (the in-flight transfer no longer
 * completes, so no callback is pending to race the SPI_close).  A real
 * SPI_close + SPI_open is required: a flag the (now-dead) callback would have to
 * read can never be acted on.  After this the slave drives 0xA5 again and the
 * host's next poll lands cleanly. */
/* #1610 diagnosis: which phase is the machine parked in?  In POLLED mode the FIRST
 * OTA_WRITE lands (dev_cursor=256) and every later one fails, which is a phase
 * machine that never returned to PH_REQ_HEADER after a payload frame -- not a
 * timing race (a polled-scoped 5 ms settle applied and changed nothing).  Reported
 * through OTA_STATUS reserved[2] so it is readable without a UART on the CC35. */
uint8_t bridge_transport_spi_phase(void)
{
	return (uint8_t)phase;
}

void bridge_transport_spi_hw_reinit(void)
{
	/* OTA update mode with a LIVE handle: nothing was ever torn down, so there is
	 * nothing to re-establish -- and re-rolling spi_open_and_arm()'s SPI_open retry
	 * dice can only lose.  Worse, the SPI_close below would drop the handle under a
	 * task blocked inside a polled SPI_transfer (the poll-task deadlock).  This one
	 * guard covers every caller across cc3501e_hw_ti{,_ble,_wifi,_ota}.c and
	 * worker.c.  NEVER re-add SPI_transferCancel here -- see the note below.
	 *
	 * spi == NULL is the ONE exception, and it must stay one: every SPI_open retry
	 * failed, so there is no handle for anything to block inside and no transfer to
	 * cut short -- SPI_close is skipped below and this degenerates to a plain
	 * re-open.  That is cc3501e_hw_tick()'s dead-slave self-heal
	 * (bridge_transport_spi_is_dead), and it is the only recovery update mode has:
	 * blanket-guarding it would turn a transient busy-DMA open failure at boot into
	 * a bricked update-mode boot, which is exactly the permanent wedge #1610 added
	 * that self-heal to kill.  The tick's OTHER two self-heals (resync burst, arm
	 * failure) only ever fire with a live handle, so they still no-op here. */
	if (bridge_transport_spi_polled() && spi != NULL) {
		return;
	}
	if (spi != NULL) {
		/* NO SPI_transferCancel here.  I added one on audit advice ("closing a
		 * live handle leaks the armed transfer's DMA") and it HUNG THE BRIDGE:
		 * with a bisect that made ota_do_begin return immediately -- calling NO
		 * psa_fwu function at all -- BEGIN still never returned and the CC35 went
		 * unreachable until a WIFI_EN/nRESET cycle (silicon 2026-08-21).  The
		 * suspend path can cancel safely because it runs BEFORE a radio op with
		 * the host held off; here the host may be mid-transaction, and cancelling
		 * an armed CALLBACK transfer from this context does not return.  The
		 * theoretical DMA leak is the lesser evil, and spi_open_and_arm's retry
		 * budget already covers a momentarily-busy channel. */
		SPI_close(spi);
		spi = NULL;
	}
	spi_open_and_arm();
}

/* Quiesce the bridge slave + RELEASE its DMA (ch12/13) for the DURATION of a radio op
 * that re-arbitrates the shared HIF DMA (BLE-controller enable).  Unlike reinit (which
 * recovers AFTER an op), this runs BEFORE the op so the bridge SPI's DMA is not a live
 * second client contending with the HIF handshake the NWP must command-complete.  On
 * this rev there is NO host-driver mutex serialising bridge-HIF vs BLE-enable-HIF use
 * (ctrlCmdFw_LockHostDriver is a no-op), so the bridge MUST stand down explicitly.
 * SPI_transferCancel drops the armed CALLBACK transfer (frees its DMA) before SPI_close;
 * the worker calls bridge_transport_spi_hw_reinit() after the op to bring the slave back
 * (the host poll-retries on IO across the down-window). */
void bridge_transport_spi_hw_suspend(void)
{
	/* OTA update mode: no radio op ever runs (the update-mode loop does nothing
	 * but service the bridge and pump OTA), so there is nothing to stand down for
	 * -- and both the SPI_transferCancel and the SPI_close below are calls proven
	 * to hang or deadlock a polled slave. */
	if (bridge_transport_spi_polled()) {
		return;
	}
	if (spi != NULL) {
		SPI_transferCancel(spi); /* cancel the in-flight phase transfer + its DMA */
		SPI_close(spi);
		spi = NULL;
	}
	phase = PH_REQ_HEADER;
}

void bridge_transport_spi_hw_init(void)
{
	spi_open_and_arm();
}

/* True while the SPI slave has no handle (every SPI_open retry failed).  The
 * tick's desync and arm-failure self-heals both rely on counters that only move
 * when a handle exists, so this is the only signal for that state (#1610). */
bool bridge_transport_spi_is_dead(void)
{
	return g_spi_open_failed != 0u;
}
