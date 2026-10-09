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

uint32_t alif_se_hw_vtor_read(void)
{
	return SCB->VTOR;
}

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

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
static uint32_t _arm_cyc; /* DWT CYCCNT when the wake timer was armed */
#endif

/* LPTIMER registers for the diagnostics and the pre-WFI verification: channel block
 * base + channel * 0x14 + {LOADCOUNT 0x00, CURRENTVAL 0x04, CONTROLREG 0x08}; block-wide
 * RAWINTSTATUS at +0xA8, INTSTATUS at +0xA0 (Alif DFP soc.h LPTIMER_Type, the same
 * offsets counter_alif_lptimer.h transcribes). */
#define HW_LPT_CH      (DT_REG_ADDR(HW_WAKE_TIMER_NODE) + HW_LPTIMER_CHANNEL * 0x14u)
#define HW_LPT_LOAD    (HW_LPT_CH + 0x00u)
#define HW_LPT_CUR     (HW_LPT_CH + 0x04u)
#define HW_LPT_CTRL    (HW_LPT_CH + 0x08u)
#define HW_LPT_RAWINT  (DT_REG_ADDR(HW_WAKE_TIMER_NODE) + 0xA8u)
#define HW_LPT_CTRL_EN BIT(0)
#define HW_LPT_CTRL_IM BIT(2) /* interrupt MASK: 1 = masked */

/* Arm the wake timer and prove it is live, with interrupts off, immediately before the
 * WFI: a timer that already fired (the SE calls took longer than the interval) or that
 * is not counting / has its interrupt masked would let the SoC sleep with no wake source
 * (bench U8c: the 500 ms wake never fired).  BUSY = already fired, IO = not live. */
static alp_status_t wake_timer_arm_locked(uint32_t ticks)
{
	alp_status_t s = alif_se_hw_wake_timer_arm(ticks);

	if (s != ALP_OK) {
		return s;
	}
	/* A cycle counter bounds the liveness poll below (and times the arm -> WFI gap in the
	 * bench diagnostics).  The caller restores DEMCR / DWT_CTRL on a refusal. */
	DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
	_arm_cyc = DWT->CYCCNT;
#endif
	if ((sys_read32(HW_LPTIMER_INTSTATUS) & BIT(HW_LPTIMER_CHANNEL)) != 0u ||
	    (sys_read32(HW_LPT_RAWINT) & BIT(HW_LPTIMER_CHANNEL)) != 0u) {
		return ALP_ERR_BUSY;
	}
	uint32_t ctrl = sys_read32(HW_LPT_CTRL);

	if ((ctrl & HW_LPT_CTRL_EN) == 0u || (ctrl & HW_LPT_CTRL_IM) != 0u) {
		return ALP_ERR_IO;
	}
	/* "Loaded" is not "non-zero": on this silicon CURRENTVAL reads 0xFFFFFFFF while the
	 * timer is not loaded (cold boot, a timer never started; bench U8d) and 0 before the
	 * 32 kHz domain has seen its first edge after ENABLE (~30 us).  So both are "not
	 * loaded".  Poll until CURRENTVAL is a real count (<= LOADCOUNT), then require a
	 * second read strictly smaller than the first: only a counting timer does that.  The
	 * whole poll is bounded to ~10 LF ticks (~300 us); only a timeout means "not counting". */
	const uint32_t bound = (sys_clock_hw_cycles_per_sec() / 1000000u) * 305u;
	const uint32_t t0    = DWT->CYCCNT;
	const uint32_t load  = sys_read32(HW_LPT_LOAD);
	uint32_t       first = 0u;
	bool           have  = false;

	for (;;) {
		uint32_t cur = sys_read32(HW_LPT_CUR);

		if (cur != 0u && cur != 0xFFFFFFFFu && cur <= load) {
			if (!have) {
				first = cur;
				have  = true;
			} else if (cur < first) {
				break; /* counting down */
			}
		}
		if ((DWT->CYCCNT - t0) > bound) {
			return ALP_ERR_IO;
		}
	}
	/* A very short interval could have fired while polling. */
	if ((sys_read32(HW_LPTIMER_INTSTATUS) & BIT(HW_LPTIMER_CHANNEL)) != 0u) {
		return ALP_ERR_BUSY;
	}
	return ALP_OK;
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

static alp_status_t wake_timer_arm_locked(uint32_t ticks)
{
	(void)ticks;
	return ALP_ERR_NOSUPPORT;
}
#endif

/* ---- Bench diagnostics (CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH) --------------
 *
 * Two raw register snapshots in BKRAM, which survives the STOP they document:
 *   PRE  taken with interrupts off immediately before the WFI;
 *   BOOT taken by the earliest init hook (PRE_KERNEL_1, priority 0) of the next boot,
 *        before the SoM restore (POST_KERNEL 0), the I2C pass and any driver init.
 * main() prints both.  Word index -> register:
 *    0 LPTIMER CONTROLREG   1 LPTIMER RAWINTSTATUS  2 LPTIMER INTSTATUS
 *    3 LPTIMER LOADCOUNT    4 LPTIMER CURRENTVAL    5 ANA WKUP_CTRL  (0x1A60A008)
 *    6 VBAT TIMER_CLKSEL    7 ANA MISC_CTRL         8 LPRTC CCVR (+0x00)
 *    9 LPRTC CMR (+0x04)   10 VBAT RTC_CLK_EN as found (set here when clear)
 *   11 NVIC ISER1 (IRQ 60 = bit 28)    12 NVIC ISPR1
 *   13 DWT CYCCNT delta, LPTIMER arm -> snapshot (PRE only; 0 in BOOT)
 *   14 VBAT RET_CTRL        15 ANA VBAT_ANA_REG1   16 STOP_MODE (0x1A60F000)
 *   17 AON RTSS_HE_CTRL     18 PWRMODCTL CPDLPSTATE  19 AON RTSS_HE_RESET (0x1A604014)
 *   20 CGU OSC_CTRL (0x1A602000)  21 PLL_LOCK_CTRL (+4)  22 PLL_CLK_SEL (+8)
 *   23 ESCLK_SEL (+0x10)   24 CLK_ENA (+0x14)     25 CLKCTL_SYS ACLK_CTRL (0x1A010820)
 *   26 AON SYSTOP_CLK_DIV (0x1A604020)   27 CLKCTL_PER_SLV UART_CTRL (0x4902F008)
 *   28..35 NVIC ISPR0..7   36 NVIC ISER0   37 SysTick CTRL   38 SCB VTOR
 *   39 RTSS_HE_RESET re-read after the W1C acknowledge, bit 31 = "bit 0 was set and
 *      cleared" -- tracks bit 0 ONLY; bit 4 (0x10, probably "HE domain powered from off")
 *      and the rest are in the raw word 19 (BOOT only; written by
 *      alp_som_power_reset_syndrome_take)
 *   40 clock restore: 1 = triggered, 0 = hardware state was healthy, 2 = failed
 *   41 clock restore: se_service_set_run_cfg return code (0 if not triggered)
 *   42 PLL_LOCK_CTRL after the restore   43 PLL_CLK_SEL after   44 OSC_CTRL after
 *   45 ACLK_CTRL after                   46 RUN memory_blocks the SE reports after
 *   47 CLKCTL_SYS ACLK_DIV0 (0x1A010824) 48 LPGPIO EXT_PORTA (0x42002050)
 *   49 AON RTSS_HE_LPPERI_CKEN (0x1A60401C)
 *   50 STOP_MODE read back after the STAT clear (bit 4 must be 0)
 *   51 image identity (CRC32 of .text)   52..55 reserved
 * PRE words 17 and 37 are patched after the pre-WFI writes; word 25 and 27 are read
 * separately (riskier blocks) so a fault there cannot lose the rest of the slot.
 */
#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
static void diag_capture(uint32_t w[ALP_SOM_PD_DIAG_WORDS], bool with_lptimer)
{
	for (unsigned i = 0; i < ALP_SOM_PD_DIAG_WORDS; ++i) {
		w[i] = 0u;
	}
#if DT_HAS_CHOSEN(alp_power_wake_timer) && DT_NODE_HAS_STATUS_OKAY(DT_CHOSEN(alp_power_wake_timer))
	w[0] = sys_read32(HW_LPT_CTRL);
	w[1] = sys_read32(HW_LPT_RAWINT);
	w[2] = sys_read32(HW_LPTIMER_INTSTATUS);
	w[3] = sys_read32(HW_LPT_LOAD);
	w[4] = sys_read32(HW_LPT_CUR);
	if (with_lptimer) {
		w[13] = DWT->CYCCNT - _arm_cyc;
	}
#endif
	w[5]  = sys_read32(HW_ANA_BASE + 0x08u);
	w[6]  = sys_read32(HW_VBAT_BASE + 0x04u);
	w[7]  = sys_read32(HW_ANA_MISC);
	w[10] = sys_read32(HW_VBAT_BASE + 0x10u);
#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
	/* Bench only: the LPRTC counter needs RTC_CLK_EN; enable it when clear (w[10] is how it
	 * was).  A product build leaves the VBAT register alone and does not read the counter. */
	if ((w[10] & 1u) == 0u) {
		sys_write32(w[10] | 1u, HW_VBAT_BASE + 0x10u);
	}
	w[8] = sys_read32(0x42000000u);
	w[9] = sys_read32(0x42000004u);
#endif
	w[11] = NVIC->ISER[1];
	w[12] = NVIC->ISPR[1];
	w[14] = sys_read32(HW_RET_CTRL);
	w[15] = sys_read32(HW_ANA_REG1);
	w[16] = sys_read32(0x1A60F000u);
	w[17] = sys_read32(HW_RTSS_HE_CTRL);
	w[18] = PWRMODCTL->CPDLPSTATE;
	w[19] = sys_read32(0x1A604014u);
	w[20] = sys_read32(0x1A602000u);
	w[21] = sys_read32(0x1A602004u);
	w[22] = sys_read32(0x1A602008u);
	w[23] = sys_read32(0x1A602010u);
	w[24] = sys_read32(0x1A602014u);
	w[26] = sys_read32(0x1A604020u);
	for (unsigned i = 0; i < 8u; ++i) {
		w[28 + i] = NVIC->ISPR[i];
	}
	w[36] = NVIC->ISER[0];
	w[37] = SysTick->CTRL;
	w[38] = SCB->VTOR;
	w[48] = sys_read32(0x42002050u); /* LPGPIO EXT_PORTA: DW GPIO base + 0x50 */
	w[49] = sys_read32(0x1A60401Cu); /* AON RTSS_HE_LPPERI_CKEN */
}

static void diag_capture_to_slot(unsigned slot, bool with_lptimer)
{
	uint32_t w[ALP_SOM_PD_DIAG_WORDS];

	/* AON / LPTIMER / VBAT / CGU / NVIC words first, saved at once ... */
	diag_capture(w, with_lptimer);
	alp_som_pd_diag_save(slot, w, ALP_SOM_PD_DIAG_WORDS);
	/* ... then the two reads in other clock domains, each saved on its own: a bus fault
	 * on either leaves everything above intact in BKRAM. */
	alp_som_pd_diag_patch(slot, 25u, sys_read32(0x1A010820u)); /* CLKCTL_SYS ACLK_CTRL */
	alp_som_pd_diag_patch(slot, 27u, sys_read32(0x4902F008u)); /* PER_SLV UART_CTRL */
	alp_som_pd_diag_patch(slot, 47u, sys_read32(0x1A010824u)); /* CLKCTL_SYS ACLK_DIV0 */
}

/* Earliest possible: before the SoM restore (POST_KERNEL 0), before the counter driver
 * initialises (which clears the LPTIMER state) and before any console output. */
static int diag_boot_snapshot(void)
{
	diag_capture_to_slot(ALP_SOM_PD_DIAG_BOOT, false);
	return 0;
}
SYS_INIT(diag_boot_snapshot, PRE_KERNEL_1, 0);
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

static const char *_enter_reason = "none";

const char *alif_se_hw_enter_reason(void)
{
	return _enter_reason;
}

alp_status_t alif_se_hw_enter_ewic(bool rtc_int, uint32_t lptimer_ticks)
{
	uint32_t     orig_ctrl    = sys_read32(HW_RTSS_HE_CTRL);
	uint32_t     orig_cppwr   = ICB->CPPWR;
	uint32_t     orig_ccr     = SCB->CCR;
	uint32_t     orig_mscr    = MEMSYSCTL->MSCR;
	uint32_t     orig_demcr   = DCB->DEMCR;
	uint32_t     orig_dwt     = DWT->CTRL;
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

	_enter_reason = "int_pad";
	if (rtc_int) {
		armed = rtc_int_open_locked();
	}
	_enter_reason = (armed == ALP_ERR_BUSY) ? "rtc_int_already_asserted" : "int_pad";
	/* The LPTIMER is armed LAST, here, so no SE call, readback or printk can outlast it. */
	if (armed == ALP_OK && lptimer_ticks != 0u) {
		armed         = wake_timer_arm_locked(lptimer_ticks);
		_enter_reason = (armed == ALP_ERR_BUSY) ? "lptimer_already_fired"
		                : (armed == ALP_ERR_IO) ? "lptimer_not_live_or_masked"
		                                        : "lptimer_arm";
	}
#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
	/* Before any refusal return, so a refused sleep still leaves its data. */
	diag_capture_to_slot(ALP_SOM_PD_DIAG_PRE, true);
#endif
	if (armed != ALP_OK) {
		if (lptimer_ticks != 0u) {
			alif_se_hw_wake_timer_disarm();
		}
		alif_se_hw_rtc_int_disarm();
		DWT->CTRL  = orig_dwt; /* the arm turned the cycle counter on */
		DCB->DEMCR = orig_demcr;
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

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
	/* Words 17 (RTSS_HE_CTRL) and 37 (SysTick CTRL) were captured before the writes
	 * above; patch in what the hardware holds going into the WFI. */
	alp_som_pd_diag_patch(ALP_SOM_PD_DIAG_PRE, 17u, sys_read32(HW_RTSS_HE_CTRL));
	alp_som_pd_diag_patch(ALP_SOM_PD_DIAG_PRE, 37u, SysTick->CTRL);
#endif

	/* Does not return when the power is removed: the wake is a cold boot. */
	__WFI();

	/* A wake source fired before power was removed.  Put everything back, the wake
	 * pad first, while interrupts are still off. */
	SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
	sys_write32(orig_ctrl, HW_RTSS_HE_CTRL); /* the WIC bits and COLD_WAKEUP as found */
	alif_se_hw_rtc_int_disarm();

	MEMSYSCTL->MSCR |= orig_mscr & (MEMSYSCTL_MSCR_ICACTIVE_Msk | MEMSYSCTL_MSCR_DCACTIVE_Msk);
	SCB->CCR      = orig_ccr;
	DWT->CTRL     = orig_dwt;
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

/* AON.RTSS_HE_RESET (0x1A604014), M55-HE reset status: RESETSYNDROME [5:0], set by hardware
 * and cleared by software with a 1 (E8 SVD, oneToClear).  Values the SVD names: 0 = POR or
 * Secure-Enclave-initiated reset, 1 = the NSRST pin was asserted, 4 = a reset request to
 * the power domain.  Read and acknowledge here, so the next boot reads its own cause
 * rather than a stale bit; the earliest bench snapshot (word 19) has the raw value first. */
uint32_t alp_som_power_reset_syndrome_take(void)
{
	uint32_t v = sys_read32(0x1A604014u) & 0x3Fu;

	if (v != 0u) {
		sys_write32(v, 0x1A604014u);
	}
#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
	/* The SVD marks the field write-only with reset value 1: record what a re-read
	 * shows, and whether bit 0 actually cleared. */
	uint32_t v2 = sys_read32(0x1A604014u) & 0x3Fu;

	alp_som_pd_diag_patch(
	    ALP_SOM_PD_DIAG_BOOT, 39u, v2 | (((v & 1u) != 0u && (v2 & 1u) == 0u) ? BIT(31) : 0u));
#endif
	return v;
}

/* Can bit 0 (NSRST) of the syndrome be trusted as a pin-reset marker?  Not if it is
 * stuck: set, and still set after a W1C acknowledge (a write-only field with reset
 * value 1 would look like that).  Probed BEFORE the sleep and carried in the record, so
 * a stale bit can never turn a genuine SE wake into an "aborted sleep".  A bit that
 * reads 0 is trusted (it can only be set by a reset from now on). */
bool alp_som_power_reset_syndrome_trusted(void)
{
	uint32_t v = sys_read32(0x1A604014u) & 0x3Fu;

	if ((v & 1u) == 0u) {
		return true;
	}
	sys_write32(v, 0x1A604014u);
	return (sys_read32(0x1A604014u) & 1u) == 0u;
}

/* ---- LPRTC, CGU and STOP_MODE_STAT -------------------------------------------- */

/* LPRTC CCVR (+0x00, the counter).  Needs VBAT.RTC_CLK_EN; enabled when clear.  The unit
 * is NOT seconds: the bench counted 155 ticks over a 76 s sleep (~2 Hz, LFRC / 2^14), so
 * callers use it only as a coarse, conservative elapsed-time bound. */
uint32_t alif_se_hw_lprtc_ccvr(void)
{
#ifndef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
	return 0u; /* product: the RV-3028 is the elapsed-time witness; RTC_CLK_EN stays untouched */
#else
	uint32_t en = sys_read32(HW_VBAT_BASE + 0x10u);

	if ((en & 1u) == 0u) {
		sys_write32(en | 1u, HW_VBAT_BASE + 0x10u);
	}
	return sys_read32(0x42000000u);
#endif
}

/* CGU registers (base 0x1A602000, Alif DFP soc.h CGU_BASE): OSC_CTRL +0x00, PLL_LOCK_CTRL
 * +0x04, PLL_CLK_SEL +0x08.  CLKCTL_SYS ACLK_CTRL 0x1A010820. */
uint32_t alif_se_hw_cgu_read(unsigned which)
{
	switch (which) {
	case ALIF_SE_CGU_OSC_CTRL:
		return sys_read32(0x1A602000u);
	case ALIF_SE_CGU_PLL_LOCK_CTRL:
		return sys_read32(0x1A602004u);
	case ALIF_SE_CGU_PLL_CLK_SEL:
		return sys_read32(0x1A602008u);
	case ALIF_SE_CGU_ACLK_CTRL:
	default:
		return sys_read32(0x1A010820u);
	}
}

/* VBAT_STOP_MODE_REG (0x1A60F000): bit 0 STOP_MODE_CTRL ("SW sets this bit to enter stop
 * mode"), bit 4 STOP_MODE_STAT (sticky until acknowledged).  HAZARD: a write with bit 0 set
 * enters stop mode, so this is a plain write of exactly the STAT bit and never a
 * read-modify-write (a read-modify-write would write back bit 0 if it reads 1). */
#define HW_STOP_MODE_REG DT_REG_ADDR(DT_NODELABEL(stop_mode))
BUILD_ASSERT(HW_STOP_MODE_REG == 0x1A60F000u, "VBAT_STOP_MODE_REG moved");
#define HW_STOP_MODE_CTRL     BIT(0)
#define HW_STOP_MODE_STAT     BIT(4)
#define HW_STOP_MODE_STAT_W1C 0x00000010u
BUILD_ASSERT((HW_STOP_MODE_STAT_W1C & HW_STOP_MODE_CTRL) == 0u,
             "the STOP_MODE_STAT acknowledge must never set STOP_MODE_CTRL (bit 0)");
BUILD_ASSERT(HW_STOP_MODE_STAT_W1C == HW_STOP_MODE_STAT, "acknowledge exactly the STAT bit");

bool alp_som_power_stop_mode_stat_clear(void)
{
	static bool logged;

	sys_write32(HW_STOP_MODE_STAT_W1C, HW_STOP_MODE_REG);

	uint32_t rb = sys_read32(HW_STOP_MODE_REG);

	alp_som_pd_diag_patch(ALP_SOM_PD_DIAG_BOOT, 50u, rb);
	if ((rb & HW_STOP_MODE_STAT) != 0u) {
		if (!logged) {
			logged = true;
			printk("alif_se_power: STOP_MODE_STAT did not clear (reads 0x%08x); relying on the "
			       "image identity instead\n",
			       (unsigned)rb);
		}
		return false;
	}
	return true;
}

uint32_t alif_se_hw_lpgpio_ext_porta(void)
{
	return sys_read32(0x42002050u);
}

uint32_t alif_se_hw_lpperi_cken(void)
{
	return sys_read32(0x1A60401Cu);
}

/* PRE_KERNEL_1 runs before the system clock driver (PRE_KERNEL_2) starts SysTick, so
 * k_busy_wait() -- which waits on the cycle counter the kernel derives from it -- must not
 * be used there.  DWT CYCCNT only needs TRCENA + CYCCNTENA. */
uint32_t alif_se_hw_cycles(void)
{
	if ((DCB->DEMCR & DCB_DEMCR_TRCENA_Msk) == 0u) {
		DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
	}
	if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0u) {
		DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	}
	return DWT->CYCCNT;
}
