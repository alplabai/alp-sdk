/* src/platform/he_fault.c -- the glue for src/ipc/tr_he_fault.h: why did the M55-HE restart?
 *
 *  - Every printk() character is also teed into a ring in the shared SRAM0 page, so the console
 *    text from before a reboot survives (ram_console_buf lives in .bss, which the startup code
 *    zeroes, and ram_console restarts at offset 0).
 *  - k_sys_fatal_error_handler() records reason / PC / LR / CFSR / HFSR / MMFAR / BFAR first, then
 *    (last resort, TR_HE_FAULT_SOC_RESET) asks the Secure Enclave to reset the whole SoC, unless
 *    tr_reset_guard.h says the fault repeats on every boot (the third quick one in a row halts). A
 *    fault that does not reach the reset halts exactly as Zephyr's default does (CONFIG_REBOOT is
 *    off: this core never restarts itself, so a restart WITHOUT a record came from outside it).
 *
 *    The reset request is sent DIRECTLY, bounded, not through se_service_boot_reset_soc(): that
 *    wrapper takes a mutex with a 15 s timeout (se_service.c: MUTEX_TIMEOUT) and retries a silent SE
 *    MAX_TRIES (100) times at SERVICE_TIMEOUT (10 s), all from exception context, where a held mutex
 *    would pend from handler mode and a silent SE would hold the core for ~33 minutes. This does what
 *    its polling branch does (se_service.c send_msg_to_se(), the !k_can_yield() path): one
 *    ipm_poll_out(send_dev, CH_ID = 0, &global_address, size, timeout) on the SE service's MHUv2 send
 *    node (DT: se_service's mhuv2_send_node), the request being a service_header_t with
 *    hdr_service_id = SERVICE_BOOT_RESET_SOC (se_service_boot_reset_soc(), se_service.c) in SRAM0
 *    (TR_MEM_SE_MSG; SRAM0 is its own global address, so no local_to_global()). ipm_poll_out() is the
 *    MHUv2 driver's loop-bounded poll (zephyr/drivers/ipm/ipm_arm_mhuv2.c mhuv2_poll_out(), the
 *    ipm_poll_* API from zephyr/patches/zephyr/0002): no tick, no IRQ, usable with interrupts locked.
 *    The request is not answered when it works (the SoC resets); then k_busy_wait() bounds the wait
 *    to SE_RESET_WAIT_US and the core halts if the SoC is still running. Assumes the SE was
 *    synchronised earlier (this image calls the SE service during init); not bench-verified.
 *  - At the next boot the record and the old console tail are printed, then the record is re-armed.
 *    (No Alif reset-status register is read: it is not documented in hal_alif, and an unverified
 *    peripheral read at boot is not worth a possible bus fault.)
 *
 */
#include <zephyr/device.h>
#include <zephyr/fatal.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk-hooks.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>

#include "../ipc/tr_he_fault.h"
#include "../ipc/tr_memmap.h"
#include "../ipc/tr_reset_guard.h"

/* The SoC reset needs the SE client's MHUv2 send node; without it (or with the option off) a
 * fatal error halts, as Zephyr's default does. */
#if defined(TR_HE_FAULT_SOC_RESET) && TR_HE_FAULT_SOC_RESET && \
    defined(CONFIG_HAS_ALIF_SE_SERVICES) && DT_NODE_EXISTS(DT_NODELABEL(se_service))
#define HE_SOC_RESET 1
#include <zephyr/drivers/ipm.h>
#include <services_lib_ids.h>
#include <services_lib_protocol.h>
#else
#define HE_SOC_RESET 0
#endif

_Static_assert(TR_MEM_HE_FAULT_SIZE == sizeof(tr_he_fault_t),
               "TR_MEM_HE_FAULT_SIZE != sizeof(tr_he_fault_t)");
_Static_assert(TR_MEM_HE_RESET_GUARD_SIZE == sizeof(tr_reset_guard_t),
               "TR_MEM_HE_RESET_GUARD_SIZE != sizeof(tr_reset_guard_t)");

/* Cortex-M system control block (architecture-defined addresses). */
#define SCB_CFSR  0xE000ED28UL
#define SCB_HFSR  0xE000ED2CUL
#define SCB_MMFAR 0xE000ED34UL
#define SCB_BFAR  0xE000ED38UL

#define REC   ((tr_he_fault_t *)(uintptr_t)TR_MEM_HE_FAULT)
#define GUARD ((tr_reset_guard_t *)(uintptr_t)TR_MEM_HE_RESET_GUARD)

#if HE_SOC_RESET
#define SE_RESET_CH_ID   0U /* se_service.c CH_ID */
#define SE_RESET_POLL_MS 100U
#define SE_RESET_WAIT_US 100000U /* then give up: 2 x the bounds, <= 200 ms in all */

_Static_assert(TR_MEM_SE_MSG_SIZE == sizeof(service_header_t),
               "TR_MEM_SE_MSG_SIZE != service_header_t");

/* One bounded reset request. Returns only if the SoC was not reset. */
static void he_soc_reset_request(void)
{
	const struct device *send_dev =
	    DEVICE_DT_GET_OR_NULL(DT_PHANDLE(DT_NODELABEL(se_service), mhuv2_send_node));
	volatile service_header_t *msg = (volatile service_header_t *)(uintptr_t)TR_MEM_SE_MSG;
	uint32_t                   global_address = (uint32_t)TR_MEM_SE_MSG;

	if (send_dev == NULL || !device_is_ready(send_dev)) {
		return;
	}
	msg->hdr_service_id = SERVICE_BOOT_RESET_SOC;
	msg->hdr_flags      = 0;
	msg->hdr_error_code = 0;
	msg->hdr_padding    = 0;
	barrier_dsync_fence_full();
	if (ipm_poll_out(send_dev,
	                 SE_RESET_CH_ID,
	                 &global_address,
	                 (int)sizeof(*msg),
	                 K_MSEC(SE_RESET_POLL_MS)) == 0) {
		k_busy_wait(SE_RESET_WAIT_US); /* the SE resets the SoC; nothing to wait for otherwise */
	}
}
#endif

static printk_hook_fn_t prev_hook;

static int tee_hook(int c)
{
	tr_he_fault_tail_put(REC, (char)c);
	return prev_hook != NULL ? prev_hook(c) : c;
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	tr_he_fault_record(REC,
	                   reason,
	                   esf != NULL ? esf->basic.pc : 0u,
	                   esf != NULL ? esf->basic.lr : 0u,
	                   sys_read32(SCB_CFSR),
	                   sys_read32(SCB_HFSR),
	                   sys_read32(SCB_MMFAR),
	                   sys_read32(SCB_BFAR),
	                   k_uptime_get_32());
#if HE_SOC_RESET
	if (tr_reset_guard_allow(GUARD, k_uptime_get_32())) {
		printk("he-fault: fatal error %u, asking the SE to reset the SoC\n", reason);
		he_soc_reset_request();
		printk("he-fault: the SoC was not reset, halting\n");
	} else {
		printk("he-fault: fatal errors keep coming right after each boot, halting\n");
	}
#endif
	/* Zephyr's default: log and halt. */
	k_fatal_halt(reason);
}

static int he_fault_init(void)
{
	static tr_he_fault_snap_t snap; /* the previous boot's record, before the ring is re-armed */

	(void)tr_he_fault_take(REC, &snap);
	prev_hook = __printk_get_hook();
	__printk_hook_install(tee_hook);

	if (snap.fault) {
		printk("he-fault: the previous boot ended in a fatal error at uptime %u ms: reason=%u "
		       "pc=0x%08x lr=0x%08x CFSR=0x%08x HFSR=0x%08x MMFAR=0x%08x BFAR=0x%08x\n",
		       snap.uptime_ms,
		       snap.reason,
		       snap.pc,
		       snap.lr,
		       snap.cfsr,
		       snap.hfsr,
		       snap.mmfar,
		       snap.bfar);
	} else {
		printk("he-fault: no fatal-error record from the previous boot\n");
	}
	if (snap.tail_len > 0u) {
		printk("he-fault: console tail of the previous boot (%u chars) --------\n",
		       (unsigned int)snap.tail_len);
		printk("%s\nhe-fault: ---------------------------------------------------\n", snap.tail);
	}
	return 0;
}

/* Right after ram_console's own hook is in (CONFIG_CONSOLE_INIT_PRIORITY, 40). The priority has to
 * be a literal: SYS_INIT pastes it into a section name. */
BUILD_ASSERT(CONFIG_CONSOLE_INIT_PRIORITY < 41, "he_fault_init must run after the console hook");
SYS_INIT(he_fault_init, PRE_KERNEL_1, 41);
