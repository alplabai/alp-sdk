/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * Silicon half of the Alif SE STOP / STANDBY backend (#2784, unit U7): the only
 * code in the backend that reads or writes a register.  See alif_se_power_hw.h.
 *
 * STATUS: BENCH-UNVERIFIED.  Nothing here has run on silicon.  Every register
 * address and bit is traced to the Alif DFP; where the DFP is the only source
 * the citation says so.
 *
 * Register map used
 * -----------------
 *   VBAT.RET_CTRL        VBAT_BASE 0x1A609000 + 0x0C   (E8 SVD, vbat devicetree node)
 *   ANA.MISC_CTRL        ANA_BASE  0x1A60A000 + 0x00   (E8 SVD, ana devicetree node)
 *   ANA.VBAT_ANA_REG1    ANA_BASE             + 0x38   (E8 SVD)
 *   AON.RTSS_HE_CTRL     AON_BASE  0x1A604000 + 0x10   (Alif DFP soc.h AON_Type)
 *       [0]  COLD_WAKEUP  "set to wake the M55-HE power domain during cold boot
 *                          or waking up from stop mode, clear it once the
 *                          power-on sequence is complete to enable dynamic power
 *                          transitions of M55-HE" (E8 SVD)
 *       [9:8] WIC         WICCONTROL[1:0]: bit8 = WIC enable, bit9 = IWIC select
 *                          (0 = EWIC)  (Alif DFP Device/core/common/source/pm.c)
 *   LPTIMER INTSTATUS    LPTIMER base + 0xA0, bit <channel>  (Alif DFP soc.h)
 *
 * The entry sequence follows the DFP's pm_core_enter_deep_sleep_request_subsys_off()
 * (Device/core/common/source/pm.c) step for step, with the one deliberate change
 * that RTSS_HE_CTRL is read-modify-written instead of overwritten.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#include <cmsis_core.h>

#include "alif_se_power_hw.h"
#include "som_power.h"

/* ---- Registers ------------------------------------------------------------ */

#define HW_VBAT_BASE DT_REG_ADDR(DT_NODELABEL(vbat))
#define HW_ANA_BASE  DT_REG_ADDR(DT_NODELABEL(ana))

#define HW_RET_CTRL (HW_VBAT_BASE + 0x0Cu)
#define HW_ANA_MISC (HW_ANA_BASE + 0x00u)
#define HW_ANA_REG1 (HW_ANA_BASE + 0x38u)

BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(vbat)) >= 0x10u, "vbat node must cover RET_CTRL");
BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(ana)) >= 0x3Cu, "ana node must cover VBAT_ANA_REG1");

/* AON_BASE + 0x10: Alif DFP soc.h AON_Type RTSS_HE_CTRL; the same constant the
 * zephyr_alif fork's soc_common.h calls AON_RTSS_HE_CTRL. */
#define HW_RTSS_HE_CTRL     0x1A604010u
#define HE_CTRL_COLD_WAKEUP BIT(0)
#define HE_CTRL_WIC_EN      BIT(8) /* WICCONTROL_WIC  */
#define HE_CTRL_WIC_IWIC    BIT(9) /* WICCONTROL_IWIC: 0 selects the EWIC */
#define HE_CTRL_WIC_MASK    (HE_CTRL_WIC_EN | HE_CTRL_WIC_IWIC)

#define HW_DHCSR           0xE000EDF0u
#define HW_DHCSR_C_DEBUGEN BIT(0)
#define PM_LPSTATE_OFF     3u /* DFP pm.c PM_LPSTATE_OFF */
#define PM_CPDLPSTATE_ALL_OFF \
	(PWRMODCTL_CPDLPSTATE_CLPSTATE_Msk | PWRMODCTL_CPDLPSTATE_ELPSTATE_Msk | \
	 PWRMODCTL_CPDLPSTATE_RLPSTATE_Msk)

/* LPGPIO combined interrupt: the individual LPGPIO lines (IRQ 171..178) do not reach
 * the EWIC, only the combined one does (zephyr_alif soc/alif/common/rtss/power.c
 * LPGPIO_COMB_IRQ_IRQn, Alif DFP pm.c). */
#define HW_LPGPIO_COMB_IRQ 57

uint32_t alif_se_hw_reg_read(alif_se_reg_t reg)
{
	switch (reg) {
	case ALIF_SE_REG_RET_CTRL:
		return sys_read32(HW_RET_CTRL);
	case ALIF_SE_REG_ANA_REG1:
		return sys_read32(HW_ANA_REG1);
	case ALIF_SE_REG_ANA_MISC:
	default:
		return sys_read32(HW_ANA_MISC);
	}
}

void alif_se_hw_reg_write(alif_se_reg_t reg, uint32_t value)
{
	switch (reg) {
	case ALIF_SE_REG_RET_CTRL:
		sys_write32(value, HW_RET_CTRL);
		break;
	case ALIF_SE_REG_ANA_REG1:
		sys_write32(value, HW_ANA_REG1);
		break;
	case ALIF_SE_REG_ANA_MISC:
	default:
		sys_write32(value, HW_ANA_MISC);
		break;
	}
}

/* ---- Core state ----------------------------------------------------------- */

bool alif_se_hw_debugger_attached(void)
{
	return (sys_read32(HW_DHCSR) & HW_DHCSR_C_DEBUGEN) != 0u;
}

bool alif_se_hw_dcache_active(void)
{
	return (SCB->CCR & SCB_CCR_DC_Msk) != 0u;
}

uint32_t alif_se_hw_lpstate_read(void)
{
	return PWRMODCTL->CPDLPSTATE;
}

bool alif_se_hw_lpstate_off(void)
{
	return (PWRMODCTL->CPDLPSTATE & PM_CPDLPSTATE_ALL_OFF) == PM_CPDLPSTATE_ALL_OFF;
}

/* ---- LPTIMER wake timer --------------------------------------------------- */

#if DT_HAS_CHOSEN(alp_power_wake_timer) && DT_NODE_HAS_STATUS_OKAY(DT_CHOSEN(alp_power_wake_timer))
#define HW_WAKE_TIMER_NODE DT_CHOSEN(alp_power_wake_timer)

/* LPTIMERS_INTSTATUS, LPTIMER block base + 0xA0 (Alif DFP soc.h LPTIMER_Type); the
 * same offset counter_alif_lptimer.h transcribes.  Bit n = channel n pending. */
#define HW_LPTIMER_INTSTATUS (DT_REG_ADDR(HW_WAKE_TIMER_NODE) + 0xA0u)
#define HW_LPTIMER_CHANNEL   DT_PROP(HW_WAKE_TIMER_NODE, channel)

static const struct device *wake_timer(void)
{
	const struct device *dev = DEVICE_DT_GET(HW_WAKE_TIMER_NODE);

	return device_is_ready(dev) ? dev : NULL;
}

static void wake_timer_unused_cb(const struct device *dev, uint8_t chan, uint32_t ticks, void *user)
{
	(void)dev;
	(void)chan;
	(void)ticks;
	(void)user;
}

bool alif_se_hw_wake_timer_present(void)
{
	return wake_timer() != NULL;
}

uint32_t alif_se_hw_wake_timer_hz(void)
{
	const struct device *dev = wake_timer();

	return (dev != NULL) ? counter_get_frequency(dev) : 0u;
}

alp_status_t alif_se_hw_wake_timer_arm(uint32_t ticks)
{
	const struct device *dev = wake_timer();

	if (dev == NULL) {
		return ALP_ERR_NOT_READY;
	}
	/* Drop any earlier alarm first: the driver answers a second set_alarm with -EBUSY. */
	(void)counter_cancel_channel_alarm(dev, 0);

	struct counter_alarm_cfg cfg = {
		.callback  = wake_timer_unused_cb,
		.ticks     = ticks,
		.user_data = NULL,
		.flags     = 0,
	};

	return (counter_set_channel_alarm(dev, 0, &cfg) == 0) ? ALP_OK : ALP_ERR_IO;
}

void alif_se_hw_wake_timer_disarm(void)
{
	const struct device *dev = wake_timer();

	if (dev != NULL) {
		(void)counter_cancel_channel_alarm(dev, 0);
	}
}

bool alif_se_hw_wake_timer_pending(void)
{
	return (sys_read32(HW_LPTIMER_INTSTATUS) & BIT(HW_LPTIMER_CHANNEL)) != 0u;
}
#else
bool alif_se_hw_wake_timer_present(void)
{
	return false;
}

uint32_t alif_se_hw_wake_timer_hz(void)
{
	return 0u;
}

alp_status_t alif_se_hw_wake_timer_arm(uint32_t ticks)
{
	(void)ticks;
	return ALP_ERR_NOSUPPORT;
}

void alif_se_hw_wake_timer_disarm(void)
{
}

bool alif_se_hw_wake_timer_pending(void)
{
	return false;
}
#endif

/* ---- RV-3028 /INT wake input ---------------------------------------------- */

static bool _comb_irq_opened_by_us;

static const struct gpio_dt_spec *rtc_wake_gpio(void)
{
	const struct gpio_dt_spec *spec = alp_som_power_wake_gpio(ALP_POWER_DOMAIN_RTC);

	return (spec != NULL && device_is_ready(spec->port)) ? spec : NULL;
}

bool alif_se_hw_rtc_int_present(void)
{
	return rtc_wake_gpio() != NULL;
}

int alif_se_hw_rtc_int_asserted(void)
{
	const struct gpio_dt_spec *spec = rtc_wake_gpio();

	return (spec != NULL) ? gpio_pin_get_dt(spec) : -ENODEV;
}

/* Nothing interrupt-related is switched on here: an enabled interrupt line outside
 * the interrupt lock would be live while the SE is still being programmed.  The pad
 * becomes a wake source in alif_se_hw_enter_ewic(), under the lock. */
alp_status_t alif_se_hw_rtc_int_arm(void)
{
	const struct gpio_dt_spec *spec = rtc_wake_gpio();

	if (spec == NULL) {
		return ALP_ERR_NOT_READY;
	}
	return (gpio_pin_configure_dt(spec, GPIO_INPUT) == 0) ? ALP_OK : ALP_ERR_IO;
}

/* The combined LPGPIO line has no driver in this tree (gpio_dw connects only the
 * eight individual lines, 171..178), so an enabled line 57 would reach
 * z_irq_spurious(), which is fatal.  This acknowledge-only handler gives it a
 * target; it masks the line again so a still-asserted source cannot storm. */
static void comb_isr(const void *arg)
{
	(void)arg;
	irq_disable(HW_LPGPIO_COMB_IRQ);
}

/* Interrupt lock held (PRIMASK set) by the caller. */
static alp_status_t rtc_int_open_locked(void)
{
	const struct gpio_dt_spec *spec = rtc_wake_gpio();

	if (spec == NULL) {
		return ALP_ERR_NOT_READY;
	}
	/* Falling EDGE, not level: the RV-3028 drives /INT low when the flag latches and
	 * the edge is captured by the GPIO block until acknowledged, so a short /INT
	 * pulse (tRTN1 = 7.8 ms, pulse mode) still latches the wake.  The SDK never
	 * enables pulse mode (no EEPROM / TI_TP write): /INT stays low until the flag is
	 * cleared by the wake decode. */
	if (gpio_pin_interrupt_configure_dt(spec, GPIO_INT_EDGE_FALLING) != 0) {
		return ALP_ERR_IO;
	}
	/* Same preparation as the vendor's pm_prepare_lpgpio_nvic_mask(): the combined
	 * interrupt is the one that reaches the EWIC.  Line 57 is
	 * "57 LPGPIO combined interrupt request" for the E8 M55-HE (Alif DFP
	 * Device/soc/AE822FA0E5597/include/rtss_he/soc.h:155, LPGPIO_COMB_IRQ_IRQn;
	 * the same constant as zephyr_alif soc/alif/common/rtss/power.c). */
	IRQ_CONNECT(HW_LPGPIO_COMB_IRQ, 3, comb_isr, NULL, 0);
	if (!irq_is_enabled(HW_LPGPIO_COMB_IRQ)) {
		NVIC_ClearPendingIRQ(HW_LPGPIO_COMB_IRQ);
		irq_enable(HW_LPGPIO_COMB_IRQ);
		_comb_irq_opened_by_us = true;
	}

	/* The RV-3028 countdown / alarm was started BEFORE this edge interrupt existed,
	 * and an edge that happened in between is gone.  /INT is held low until the flag
	 * is cleared, so re-read the pad now, with the edge already armed and interrupts
	 * off: an asserted /INT is a wake that already happened, and sleeping on it would
	 * never wake.  An unreadable pad is refused too. */
	int level = gpio_pin_get_dt(spec);

	if (level > 0) {
		return ALP_ERR_BUSY;
	}
	if (level < 0) {
		return ALP_ERR_IO;
	}
	return ALP_OK;
}

/* Interrupt lock held when called from the entry path; idempotent. */
void alif_se_hw_rtc_int_disarm(void)
{
	const struct gpio_dt_spec *spec = rtc_wake_gpio();

	if (spec != NULL) {
		(void)gpio_pin_interrupt_configure_dt(spec, GPIO_INT_DISABLE);
	}
	if (_comb_irq_opened_by_us) {
		irq_disable(HW_LPGPIO_COMB_IRQ);
		NVIC_ClearPendingIRQ(HW_LPGPIO_COMB_IRQ);
		_comb_irq_opened_by_us = false;
	}
}

/* ---- EWIC subsystem-off entry ---------------------------------------------- */

#if (defined(__FPU_USED) && (__FPU_USED == 1U)) || \
    (defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE > 0U))
#define HW_SAVE_FP 1
/* Only the APCS callee-preserved registers matter (DFP pm.c fp_state_t). */
typedef struct {
	double   d[8];
	uint32_t fpscr;
#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE > 0U)
	uint32_t vpr;
#endif
} hw_fp_state_t;

static inline void hw_save_fp(hw_fp_state_t *st)
{
	__asm volatile("VSTM    %0, {D8-D15}\n\t"
	               "VSTR    FPSCR, [%0, #64]\n\t"
#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE > 0U)
	               "VSTR    VPR, [%0, #68]\n\t"
#endif
	               ::"r"(st)
	               : "memory");
}

static inline void hw_restore_fp(const hw_fp_state_t *st)
{
	__asm volatile("VLDM    %0, {D8-D15}\n\t"
	               "VLDR    FPSCR, [%0, #64]\n\t"
#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE > 0U)
	               "VLDR    VPR, [%0, #68]\n\t"
#endif
	               ::"r"(st)
	               : "memory");
}
#endif

alp_status_t alif_se_hw_enter_ewic(bool rtc_int)
{
	uint32_t     orig_ctrl    = sys_read32(HW_RTSS_HE_CTRL);
	uint32_t     orig_cppwr   = ICB->CPPWR;
	uint32_t     orig_ccr     = SCB->CCR;
	uint32_t     orig_mscr    = MEMSYSCTL->MSCR;
	uint32_t     orig_demcr   = DCB->DEMCR;
	uint32_t     orig_systick = SysTick->CTRL;
	uint32_t     orig_primask = __get_PRIMASK();
	uint32_t     orig_basepri = __get_BASEPRI();
	alp_status_t armed        = ALP_OK;
#ifdef HW_SAVE_FP
	hw_fp_state_t fp;
	bool          fp_saved = false;

	if (!(orig_cppwr & ICB_CPPWR_SU10_Msk) &&
	    ((__get_CONTROL() & CONTROL_FPCA_Msk) || (FPU->FPCCR & FPU_FPCCR_LSPACT_Msk))) {
		hw_save_fp(&fp);
		fp_saved = true;
	}
#endif

	/* Interrupts off for the whole sequence, the way the DFP does it
	 * (__disable_irq(), BASEPRI 0): a pending enabled interrupt still ends the WFI,
	 * it is just not taken here.  The wake pad is armed INSIDE this section and
	 * disarmed again before it ends, so no interrupt line is ever live while the
	 * caller is unlocked. */
	__disable_irq();
	__set_BASEPRI(0);

	if (rtc_int) {
		armed = rtc_int_open_locked();
	}
	if (armed != ALP_OK) {
		alif_se_hw_rtc_int_disarm();
		__set_BASEPRI(orig_basepri);
		__set_PRIMASK(orig_primask);
		return armed;
	}

	/* PDEPU may go off only if the FP / MVE state is declared expendable. */
	ICB->CPPWR = orig_cppwr | ICB_CPPWR_SU11_Msk | ICB_CPPWR_SU10_Msk;

	/* The caller refused a live D-cache; clearing IC / DC lets PDRAMS go off. */
	SCB->CCR        = orig_ccr & ~(SCB_CCR_IC_Msk | SCB_CCR_DC_Msk);
	MEMSYSCTL->MSCR = (orig_mscr & ~(MEMSYSCTL_MSCR_ICACTIVE_Msk | MEMSYSCTL_MSCR_DCACTIVE_Msk));
	/* The kernel tick would become pending and end the WFI at once (SysTick wakes WFI
	 * even with PRIMASK set).  Stop it for the sleep. */
	SysTick->CTRL = orig_systick & ~(SysTick_CTRL_ENABLE_Msk | SysTick_CTRL_TICKINT_Msk);
	SCB->ICSR     = SCB_ICSR_PENDSTCLR_Msk;
	/* Disable the PMU / DWT trace enable so PDDEBUG can go off. */
	DCB->DEMCR = orig_demcr & ~DCB_DEMCR_TRCENA_Msk;
	__DSB();
	__ISB();

	/* Read-modify-write, never a whole-register write: WIC on, EWIC selected, and
	 * COLD_WAKEUP cleared so the M55-HE power domain may actually drop.  Every
	 * other bit keeps the value the Secure Enclave left. */
	sys_write32((orig_ctrl & ~(HE_CTRL_COLD_WAKEUP | HE_CTRL_WIC_MASK)) | HE_CTRL_WIC_EN,
	            HW_RTSS_HE_CTRL);

	SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
	__DSB();
	__ISB();

	/* Does not return when the power is removed: the wake is a cold boot. */
	__WFI();

	/* A wake source fired before power was removed.  Put everything back, the wake
	 * pad first, while interrupts are still off. */
	SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
	sys_write32(orig_ctrl, HW_RTSS_HE_CTRL); /* the WIC bits and COLD_WAKEUP as found */
	alif_se_hw_rtc_int_disarm();

	MEMSYSCTL->MSCR |= orig_mscr & (MEMSYSCTL_MSCR_ICACTIVE_Msk | MEMSYSCTL_MSCR_DCACTIVE_Msk);
	SCB->CCR      = orig_ccr;
	DCB->DEMCR    = orig_demcr;
	ICB->CPPWR    = orig_cppwr;
	SysTick->CTRL = orig_systick;
	__DSB();
	__ISB();
#ifdef HW_SAVE_FP
	if (fp_saved) {
		hw_restore_fp(&fp);
	}
#endif
	__set_BASEPRI(orig_basepri);
	__set_PRIMASK(orig_primask);
	return ALP_OK;
}
