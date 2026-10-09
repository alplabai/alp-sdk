/* src/platform/he_fault.c -- the glue for src/ipc/tr_he_fault.h: why did the M55-HE restart?
 *
 *  - Every printk() character is also teed into a ring in the shared SRAM0 page, so the console
 *    text from before a reboot survives (ram_console_buf lives in .bss, which the startup code
 *    zeroes, and ram_console restarts at offset 0).
 *  - k_sys_fatal_error_handler() records reason / PC / LR / CFSR / HFSR / MMFAR / BFAR first, then
 *    (last resort, CONFIG_HAS_ALIF_SE_SERVICES) asks the Secure Enclave to reset the whole SoC
 *    through hal_alif's se_service_boot_reset_soc() (se_services/zephyr/include/se_service.h, the
 *    SE's boot-reset service), unless tr_reset_guard.h says the fault repeats on every boot: the
 *    third quick one in a row halts. A fault that does not reach the reset halts exactly as
 *    Zephyr's default does (CONFIG_REBOOT is off: this core never restarts itself, so a restart
 *    WITHOUT a record came from outside it). The call runs in the fault's context, where
 *    se_service.c uses its polling MHU path (k_can_yield() is false); if the SE does not answer
 *    the reset, the core halts.
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
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>

#include "../ipc/tr_he_fault.h"
#include "../ipc/tr_memmap.h"
#include "../ipc/tr_reset_guard.h"

#ifdef CONFIG_HAS_ALIF_SE_SERVICES
#include <se_service.h>
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
#ifdef CONFIG_HAS_ALIF_SE_SERVICES
	if (tr_reset_guard_allow(GUARD, k_uptime_get_32())) {
		printk("he-fault: fatal error %u, asking the SE to reset the SoC\n", reason);
		(void)se_service_boot_reset_soc(); /* does not return when the SE resets the SoC */
		printk("he-fault: the SE did not reset the SoC, halting\n");
	} else {
		printk("he-fault: fatal errors keep coming right after each boot, halting\n");
	}
#endif
	/* Zephyr's default: log and halt. */
	k_fatal_halt(reason);
}

#ifdef CONFIG_HAS_ALIF_SE_SERVICES
/* Up for the guard's window: the boot is healthy, the streak of quick fatal errors is over. */
static void he_fault_healthy(struct k_timer *t)
{
	ARG_UNUSED(t);
	tr_reset_guard_healthy(GUARD);
}
K_TIMER_DEFINE(he_fault_healthy_timer, he_fault_healthy, NULL);
#endif

static int he_fault_init(void)
{
	static tr_he_fault_snap_t snap; /* the previous boot's record, before the ring is re-armed */

	(void)tr_he_fault_take(REC, &snap);
#ifdef CONFIG_HAS_ALIF_SE_SERVICES
	k_timer_start(&he_fault_healthy_timer, K_MSEC(TR_RESET_GUARD_WINDOW_MS), K_NO_WAIT);
#endif
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
