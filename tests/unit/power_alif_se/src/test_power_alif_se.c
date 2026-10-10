/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #2784 (U7): host coverage of the Alif SE STOP / STANDBY backend.
 *
 * The real src/backends/power/alif_se_power.c is compiled into this image (the
 * #include below), against a fake <se_service.h>, a recording fake of the silicon
 * seam (alif_se_power_hw.h) and fakes of the som_power API it calls.  What this
 * proves, and what it cannot:
 *
 *   proves   - the OFF profile has every member assigned (a poison pre-fill leaves
 *              no survivor), vtor_address is preserved, the masks come from the gen2
 *              header (BKRAM = bit 21, not the gen1 bit 20);
 *            - each refusal returns its documented code BEFORE any side effect;
 *            - a failure at any later step disarms the sources and restores the
 *              domains, in that order, and never reaches the EWIC entry;
 *            - the wake decode turns the record + hardware status into
 *              alp_power_boot_wake_info() fields.
 *   cannot   - anything about the silicon: whether the SE accepts the profile,
 *              whether the EWIC entry powers the subsystem down, whether BKRAM
 *              retains.  That is the bench (U8).
 */

#define CONFIG_ALP_SDK_POWER_ALIF_SE                1
#define CONFIG_ALP_SDK_POWER_ALIF_SE_RESTORE_CLOCKS 1
#define ALP_TEST_NO_SYSINIT                         1 /* the test calls clock_restore() itself */

#include "../../../../src/backends/power/alif_se_power.c"

#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#define POISON32 0xA5A5A5A5u

/* ---- Recording fakes --------------------------------------------------------- */

typedef enum {
	EV_QUIESCE = 1,
	EV_ARM_RTC_TIMER,
	EV_ARM_LPTIMER,
	EV_ARM_INT_PAD,
	EV_SAVE_RECORD,
	EV_SET_OFF_CFG,
	EV_GET_OFF_CFG,
	EV_REG_WRITE,
	EV_ENTER,
	EV_DISARM_INT_PAD,
	EV_CANCEL_RTC_TIMER,
	EV_DISARM_LPTIMER,
	EV_RESTORE,
	EV_CLEAR_STALE_WAKE,
	EV_ARM_WAKE_PADS,
	EV_DISARM_WAKE_PADS,
	EV_RELEASE_WAKE_PADS,
} ev_t;

#define EV_MAX 64
static ev_t     g_ev[EV_MAX];
static unsigned g_ev_n;

static void ev(ev_t e)
{
	if (g_ev_n < EV_MAX) {
		g_ev[g_ev_n++] = e;
	}
}

static unsigned ev_count(ev_t e)
{
	unsigned n = 0;

	for (unsigned i = 0; i < g_ev_n; ++i) {
		n += (g_ev[i] == e);
	}
	return n;
}

/* index of the first occurrence of @p e, or EV_MAX */
static unsigned ev_pos(ev_t e)
{
	for (unsigned i = 0; i < g_ev_n; ++i) {
		if (g_ev[i] == e) {
			return i;
		}
	}
	return EV_MAX;
}

/* SE */
static alp_som_pd_record_t g_rec;
static off_profile_t       g_live;
static off_profile_t       g_set;
static off_profile_t       g_readback;
static bool                g_readback_overridden;
static int                 g_get_rc, g_set_rc;
static off_profile_t       g_stored;     /* what the fake SE holds */
static alp_som_pd_record_t g_rec_at_set; /* the record as it stood at the SE call */
static uint32_t            g_se_drops_memory_bits;

static bool     g_transports_dcdc_mode;
static int      g_flip_word = -1;
static unsigned g_set_calls;

int se_service_get_off_cfg(off_profile_t *wp)
{
	ev(EV_GET_OFF_CFG);
	if (g_get_rc != 0) {
		return g_get_rc;
	}
	/* Like the real client: dcdc_mode is neither sent by set_off_cfg nor filled in
	 * here, so the caller's value survives the call (unless a future hal_alif
	 * transports it, modelled by g_transports_dcdc_mode). */
	dcdc_mode_t keep = wp->dcdc_mode;

	*wp = g_readback_overridden ? g_readback : g_stored;
	if (!g_transports_dcdc_mode) {
		wp->dcdc_mode = keep;
	}
	if (g_flip_word >= 0 && g_set_calls > 0u && g_flip_word != 2) {
		((uint32_t *)wp)[g_flip_word] ^= 1u; /* the SE hands one bit back wrong */
	}
	return 0;
}

static off_profile_t g_undo_set; /* the last profile written (the undo) */

int se_service_set_off_cfg(off_profile_t *wp)
{
	ev(EV_SET_OFF_CFG);
	if (g_set_calls++ == 0u) {
		g_rec_at_set = g_rec;
		g_set        = *wp; /* the profile of the cycle, not the undo */
	}
	g_undo_set = *wp;
	if (g_set_rc != 0) {
		return g_set_rc;
	}
	g_stored = *wp;
	g_stored.memory_blocks &= ~g_se_drops_memory_bits;
	return 0;
}

/* Silicon seam */
static bool     g_debugger, g_dcache, g_lpstate_off;
static uint32_t g_regs[3];
static uint32_t g_reg_stuck_mask[3]; /* bits a write cannot set */
static bool     g_timer_present, g_timer_pending, g_int_present;
static uint32_t g_timer_hz;
static int      g_int_level;
static uint32_t g_armed_ticks;
static int      g_arm_timer_rc, g_arm_int_rc;
static unsigned g_enter_count;
static uint8_t  g_rtc_flags; /* tentative: defined with the other som_power fakes below */
static bool     g_enter_rtc_int;
static uint32_t g_enter_pads;               /* the pads mask the entry was given */
static uint32_t g_wake_pad_mask, g_claimed; /* DT-named pads / pads the SoM wires */
static uint32_t g_pads_latched;      /* live RAW_INTSTATUS of the fake block (runtime path) */
static uint32_t g_pads_boot_latched; /* the PRE_KERNEL_1 snapshot (cold-boot path) */
static int      g_arm_pads_rc;
static uint32_t g_released_pads;
static bool     g_enter_fires; /* the fake sleep ends at once with the armed source fired */
static int      g_enter_rc;
static uint32_t g_regs_at_enter[3];

uint32_t alif_se_hw_reg_read(alif_se_reg_t reg)
{
	return g_regs[reg];
}

void alif_se_hw_reg_write(alif_se_reg_t reg, uint32_t value)
{
	ev(EV_REG_WRITE);
	g_regs[reg] = (value & ~g_reg_stuck_mask[reg]) | (g_regs[reg] & g_reg_stuck_mask[reg]);
}

bool alif_se_hw_debugger_attached(void)
{
	return g_debugger;
}

bool alif_se_hw_dcache_active(void)
{
	return g_dcache;
}

uint32_t alif_se_hw_lpstate_read(void)
{
	return g_lpstate_off ? 0x333u : 0x300u;
}

bool alif_se_hw_lpstate_off(void)
{
	return g_lpstate_off;
}

bool alif_se_hw_wake_timer_present(void)
{
	return g_timer_present;
}

uint32_t alif_se_hw_wake_timer_hz(void)
{
	return g_timer_hz;
}

void alif_se_hw_wake_timer_disarm(void)
{
	ev(EV_DISARM_LPTIMER);
}

bool alif_se_hw_wake_timer_pending(void)
{
	return g_timer_pending;
}

bool alif_se_hw_rtc_int_present(void)
{
	return g_int_present;
}

int alif_se_hw_rtc_int_asserted(void)
{
	return g_int_level;
}

alp_status_t alif_se_hw_rtc_int_arm(void)
{
	if (g_arm_int_rc != 0) {
		return (alp_status_t)g_arm_int_rc;
	}
	ev(EV_ARM_INT_PAD);
	return ALP_OK;
}

void alif_se_hw_rtc_int_disarm(void)
{
	ev(EV_DISARM_INT_PAD);
}

uint32_t alif_se_hw_wake_pad_mask(void)
{
	return g_wake_pad_mask;
}

alp_status_t alif_se_hw_wake_pads_arm(void)
{
	if (g_arm_pads_rc != 0) {
		return (alp_status_t)g_arm_pads_rc;
	}
	ev(EV_ARM_WAKE_PADS);
	return ALP_OK;
}

void alif_se_hw_wake_pads_disarm(void)
{
	ev(EV_DISARM_WAKE_PADS);
}

uint32_t alif_se_hw_wake_pads_fired(uint32_t pads)
{
	return g_pads_latched & pads;
}

void alif_se_hw_wake_pads_release(uint32_t pads)
{
	ev(EV_RELEASE_WAKE_PADS);
	g_released_pads |= pads;
	g_pads_latched &= ~pads;
}

uint32_t alif_se_hw_wake_pads_boot_latched(void)
{
	return g_pads_boot_latched;
}

uint32_t alp_som_power_lpgpio_claimed(void)
{
	return g_claimed;
}

static bool     g_lptimer_fired_at_arm;
static unsigned g_enter_lptimer_ticks;

const char *alif_se_hw_enter_reason(void)
{
	return "fake";
}

uint32_t alif_se_hw_vtor_read(void)
{
	return 0x80010400u;
}

alp_status_t alif_se_hw_enter_ewic(bool rtc_int, uint32_t pads, uint32_t lptimer_ticks)
{
	if (lptimer_ticks != 0u) {
		/* The real entry arms the timer last and proves it live, under the lock. */
		if (g_arm_timer_rc != 0) {
			return (alp_status_t)g_arm_timer_rc;
		}
		ev(EV_ARM_LPTIMER);
		g_armed_ticks = lptimer_ticks;
		if (g_lptimer_fired_at_arm) {
			return ALP_ERR_BUSY;
		}
	}
	g_enter_lptimer_ticks = lptimer_ticks;
	ev(EV_ENTER);
	g_enter_count++;
	g_enter_rtc_int = rtc_int;
	g_enter_pads    = pads;
	memcpy(g_regs_at_enter, g_regs, sizeof(g_regs));
	if (g_enter_rc != 0) {
		return (alp_status_t)g_enter_rc;
	}
	if (g_enter_fires) {
		if ((g_rec_at_set.armed_hw & ALP_SOM_ARM_LPGPIO) != 0u) {
			g_pads_latched |= pads;
		}
		if ((g_rec_at_set.armed_hw & ALP_SOM_ARM_LPTIMER) != 0u) {
			g_timer_pending = true;
		}
		if ((g_rec_at_set.armed_hw & (ALP_SOM_ARM_RTC_TIMER | ALP_SOM_ARM_RTC_INT)) != 0u) {
			g_rtc_flags = RV3028C7_WAKE_TF;
		}
	}
	return ALP_OK;
}

/* som_power API the backend calls */
static bool                      g_countdown_ready, g_rtc_int_armed;
static int                       g_quiesce_rc, g_countdown_rc;
static alp_power_domain_policy_t g_rtc_policy;
static uint32_t                  g_rtc_seconds, g_countdown_req;
static bool                      g_rtc_seconds_ok;
static uint8_t                   g_rtc_flags;
static bool                      g_rec_valid;
static alp_power_mode_t          g_quiesce_mode;

alp_status_t alp_som_power_quiesce(alp_power_mode_t mode, uint32_t *rollback_failed)
{
	(void)rollback_failed;
	ev(EV_QUIESCE);
	g_quiesce_mode = mode;
	return (alp_status_t)g_quiesce_rc;
}

static uint32_t g_restore_failed; /* domains the fake restore cannot put back */

alp_status_t alp_som_power_restore(uint32_t *failed)
{
	ev(EV_RESTORE);
	if (failed != NULL) {
		*failed = g_restore_failed;
	}
	/* Like the real one: a failed domain stays in the record, a clean restore drops it. */
	if (g_restore_failed != 0u) {
		return ALP_ERR_IO;
	}
	alp_som_pd_store_clear();
	return ALP_OK;
}

bool alp_som_pd_store_load(alp_som_pd_record_t *out)
{
	*out = g_rec;
	return g_rec_valid;
}

void alp_som_pd_store_save(alp_som_pd_record_t *rec)
{
	ev(EV_SAVE_RECORD);
	g_rec       = *rec;
	g_rec_valid = true;
}

void alp_som_pd_store_clear(void)
{
	memset(&g_rec, 0, sizeof(g_rec));
	g_rec_valid = false;
}

alp_power_domain_policy_t alp_som_power_policy(alp_power_domain_t d)
{
	(void)d;
	return g_rtc_policy;
}

alp_status_t alp_som_power_rtc_int_armed(bool *armed)
{
	*armed = g_rtc_int_armed;
	return ALP_OK;
}

alp_status_t alp_som_power_rtc_wake_service(uint8_t *flags)
{
	*flags = g_rtc_flags;
	return ALP_OK;
}

static bool g_porf;
static bool g_nsrst_trusted = true;

bool alp_som_power_reset_syndrome_trusted(void)
{
	return g_nsrst_trusted;
}
static bool    g_flags_pending;
static bool    g_fire_on_arm;
static uint8_t g_rtc_regs[6];

alp_status_t alp_som_power_rtc_flags_pending(bool *pending)
{
	*pending = g_flags_pending ||
	           ((g_rtc_regs[0] & 0x08u) != 0u && (g_rtc_regs[2] & 0x10u) != 0u) ||
	           ((g_rtc_regs[0] & 0x04u) != 0u && (g_rtc_regs[2] & 0x08u) != 0u);
	return ALP_OK;
}

alp_status_t alp_som_power_rtc_porf(bool *porf)
{
	*porf = g_porf;
	return ALP_OK;
}

alp_status_t alp_som_power_rtc_seconds(uint32_t *seconds)
{
	*seconds = g_rtc_seconds;
	return g_rtc_seconds_ok ? ALP_OK : ALP_ERR_IO;
}

static uint32_t      g_cgu[4]; /* OSC_CTRL, PLL_LOCK_CTRL, PLL_CLK_SEL, ACLK_CTRL */
static uint32_t      g_ccvr;
static uint32_t      g_ccvr_now;
static bool          g_rtc_regs_ok = true;
static unsigned      g_uf_clears;
static unsigned      g_stale_clears;
static int           g_run_set_rc;
static unsigned      g_run_set_calls;
static run_profile_t g_run_set;
static bool          g_run_set_locks_pll = true;

/* Bench build (the test's CMakeLists defines the option): the diag patch is real code in the
 * header contract, so fake it and keep what the clock restore wrote to BOOT word 40. */
#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
static uint32_t g_boot_w[ALP_SOM_PD_DIAG_WORDS];
static uint32_t g_w40_at_se_call;

void alp_som_pd_diag_patch(unsigned slot, unsigned idx, uint32_t value)
{
	if (slot == ALP_SOM_PD_DIAG_BOOT && idx < ALP_SOM_PD_DIAG_WORDS) {
		g_boot_w[idx] = value;
	}
}

void alp_som_pd_diag_invalidate(unsigned slot)
{
	(void)slot;
}
#endif

/* BKRAM seam (som_power_record.c in the product): the shadow lifecycle and the self-test. */
static unsigned g_shadow_end_calls;
static bool     g_bkram_ok;
static bool     g_shadowed;
static unsigned g_selftest_calls;

void alp_som_pd_shadow_begin(void)
{
	g_shadowed = true;
}

bool alp_som_pd_shadow_end(void)
{
	g_shadow_end_calls++;
	if (g_bkram_ok) {
		g_shadowed = false;
	}
	return g_bkram_ok;
}

bool alp_som_pd_bkram_selftest(void)
{
	g_selftest_calls++;
	return g_bkram_ok;
}

bool alp_som_pd_bkram_live(void)
{
	return !g_shadowed;
}

/* The cycle counter the PLL wait is bounded by.  Every read jumps g_cycle_step cycles, so a
 * wait bounded by CYCLES ends after a handful of reads; one bounded only by an iteration
 * count (or by a k_busy_wait that works on the host) would read it millions of times. */
static uint32_t g_cycles;
static uint32_t g_cycle_step;
static unsigned g_cycle_reads;

uint32_t alif_se_hw_cycles(void)
{
	g_cycle_reads++;
	g_cycles += g_cycle_step;
	return g_cycles;
}

uint32_t alif_se_hw_cgu_read(unsigned which)
{
	return g_cgu[which];
}

uint32_t alif_se_hw_lprtc_ccvr(void)
{
	return g_ccvr_now;
}

uint32_t alif_se_hw_lpperi_cken(void)
{
	return 0x00030F01u;
}

uint32_t alif_se_hw_lpgpio_ext_porta(void)
{
	return 0x1u;
}

alp_status_t alp_som_power_rtc_clear_stale_uf(void)
{
	g_uf_clears++;
	return ALP_OK;
}

/* Like the real one: stops the countdown enable (and, unless keep_alarm, the alarm enable), then
 * drops TF (AF) / UF; EVF etc stay. */
static bool g_last_keep_alarm;

alp_status_t alp_som_power_rtc_clear_stale_wake(bool keep_alarm)
{
	const uint8_t en = keep_alarm ? 0x10u : (0x10u | 0x08u);
	const uint8_t fl = keep_alarm ? 0x08u : (0x08u | 0x04u);

	ev(EV_CLEAR_STALE_WAKE);
	g_stale_clears++;
	g_last_keep_alarm = keep_alarm;
	g_rtc_regs[0] &= (uint8_t)~(fl | (((g_rtc_regs[2] & 0x20u) == 0u) ? 0x10u : 0u));
	g_rtc_regs[2] &= (uint8_t)~en; /* TIE (, AIE) */
	if ((g_rtc_regs[0] & 0x02u) == 0u && (g_rtc_regs[0] & 0x04u) == 0u) {
		g_int_level = 0; /* nothing left to hold /INT low */
	}
	return ALP_OK;
}

alp_status_t alp_som_power_rtc_regs(uint8_t regs[6])
{
	memcpy(regs, g_rtc_regs, 6);
	return g_rtc_regs_ok ? ALP_OK : ALP_ERR_IO;
}

int se_service_get_run_cfg(run_profile_t *pp)
{
	*pp = g_run_set;
	return 0;
}

int se_service_set_run_cfg(run_profile_t *pp)
{
	g_run_set = *pp;
	g_run_set_calls++;
#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
	g_w40_at_se_call = g_boot_w[40];
#endif
	if (g_run_set_rc == 0 && g_run_set_locks_pll) {
		g_cgu[ALIF_SE_CGU_PLL_LOCK_CTRL] = 1u;
		g_cgu[ALIF_SE_CGU_PLL_CLK_SEL]   = 0x00110111u;
		g_cgu[ALIF_SE_CGU_OSC_CTRL]      = 0x00110011u;
	}
	return g_run_set_rc;
}

bool alp_som_power_rtc_countdown_ready(void)
{
	return g_countdown_ready;
}

alp_status_t alp_som_power_rtc_countdown_start(uint32_t seconds, uint32_t *actual_s)
{
	if (g_countdown_rc != 0) {
		return (alp_status_t)g_countdown_rc;
	}
	ev(EV_ARM_RTC_TIMER);
	if (g_fire_on_arm) {
		g_rtc_regs[0] |= 0x08u; /* TF: the countdown fired after the arm */
		g_rtc_regs[2] |= 0x10u; /* TIE */
	}
	g_countdown_req = seconds;
	if (actual_s != NULL) {
		*actual_s = seconds;
	}
	return ALP_OK;
}

alp_status_t alp_som_power_rtc_countdown_cancel(void)
{
	ev(EV_CANCEL_RTC_TIMER);
	return ALP_OK;
}

alp_status_t alp_som_power_ops_policy_set(alp_power_backend_state_t *state,
                                          alp_power_domain_t         domain,
                                          alp_power_domain_policy_t  policy)
{
	(void)state;
	(void)domain;
	(void)policy;
	return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_som_power_ops_domain_info(alp_power_domain_t domain, alp_power_domain_info_t *out)
{
	(void)domain;
	(void)out;
	return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_som_power_ops_boot_wake_info(alp_power_boot_info_t *out)
{
	(void)out;
	return ALP_ERR_NOSUPPORT;
}

/* ---- Fixture ------------------------------------------------------------------ */

static alp_power_backend_state_t g_state;

static void reset_fakes(void)
{
	g_bkram_ok         = true;
	g_restore_failed   = 0u;
	g_shadowed         = false;
	g_shadow_end_calls = 0u;
	g_selftest_calls   = 0u;
#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
	alp_som_bench_knobs = (alp_som_bench_knobs_t){ 0 };
	memset(g_boot_w, 0, sizeof(g_boot_w));
	g_w40_at_se_call = 0xFFFFFFFFu;
#endif
	g_cycles      = 0u;
	g_cycle_step  = 1000000u;
	g_cycle_reads = 0u;
	memset(g_ev, 0, sizeof(g_ev));
	g_ev_n = 0;

	g_live = (off_profile_t){
		.power_domains   = 0x1u,
		.dcdc_voltage    = 825u,
		.dcdc_mode       = DCDC_MODE_PWM,
		.aon_clk_src     = CLK_SRC_LFRC,
		.stby_clk_src    = CLK_SRC_HFXO,
		.stby_clk_freq   = SCALED_FREQ_RC_STDBY_19_2_MHZ,
		.memory_blocks   = 0x00008000u,
		.ip_clock_gating = 0x1234u,
		.phy_pwr_gating  = 0x1du,
		.vdd_ioflex_3V3  = IOFLEX_LEVEL_3V3,
		.wakeup_events   = 0x50u,
		.ewic_cfg        = 0x51u,
		.vtor_address    = 0x80010000u,
		.vtor_address_ns = 0x80010400u,
	};
	g_stored              = g_live;
	g_readback_overridden = false;
	g_get_rc = g_set_rc    = 0;
	g_se_drops_memory_bits = 0;

	g_debugger    = false;
	g_dcache      = false;
	g_lpstate_off = true;
	/* cold-boot: RET_CTRL 0x0003FFF1, VBAT_ANA_REG1 0x06441f40, MISC_CTRL 0 (LFRC) */
	g_regs[ALIF_SE_REG_RET_CTRL] = 0x0003FFF1u;
	g_regs[ALIF_SE_REG_ANA_REG1] = 0x06441f40u;
	g_regs[ALIF_SE_REG_ANA_MISC] = 0x0u;
	memset(g_reg_stuck_mask, 0, sizeof(g_reg_stuck_mask));
	g_timer_present = true;
	g_timer_pending = false;
	g_timer_hz      = 32768u;
	g_int_present   = true;
	g_int_level     = 0;
	g_armed_ticks   = 0;
	g_arm_timer_rc = g_arm_int_rc = 0;
	g_enter_count                 = 0;
	g_enter_rtc_int               = false;
	g_enter_pads                  = 0u;
	g_wake_pad_mask = g_claimed = 0u;
	g_pads_latched = g_pads_boot_latched = 0u;
	g_arm_pads_rc                        = 0;
	g_released_pads                      = 0u;
	g_enter_fires                        = true;
	g_enter_rc                           = 0;
	memset(g_regs_at_enter, 0, sizeof(g_regs_at_enter));
	g_set_calls = 0;
	memset(&g_set, 0, sizeof(g_set));
	memset(&g_rec_at_set, 0, sizeof(g_rec_at_set));
	g_porf                           = false;
	g_cgu[ALIF_SE_CGU_OSC_CTRL]      = 0x00110011u;
	g_cgu[ALIF_SE_CGU_PLL_LOCK_CTRL] = 1u;
	g_cgu[ALIF_SE_CGU_PLL_CLK_SEL]   = 0x00110111u;
	g_cgu[ALIF_SE_CGU_ACLK_CTRL]     = 0x202u;
	g_ccvr = g_ccvr_now = 1000u;
	g_rtc_regs_ok       = true;
	memset(g_rtc_regs, 0, sizeof(g_rtc_regs));
	g_uf_clears         = 0u;
	g_stale_clears      = 0u;
	g_run_set_rc        = 0;
	g_run_set_calls     = 0;
	g_run_set_locks_pll = true;
	memset(&g_run_set, 0, sizeof(g_run_set));
	g_nsrst_trusted        = true;
	g_transports_dcdc_mode = false;
	g_flip_word            = -1;
	g_lptimer_fired_at_arm = false;
	g_enter_lptimer_ticks  = 0;
	g_flags_pending        = false;
	g_fire_on_arm          = false;
	g_last_keep_alarm      = false;

	g_countdown_ready = true;
	g_rtc_int_armed   = false;
	g_quiesce_rc = g_countdown_rc = 0;
	g_rtc_policy                  = ALP_POWER_DOMAIN_POLICY_AUTO;
	g_rtc_seconds                 = 1000u;
	g_rtc_seconds_ok              = true;
	g_rtc_flags                   = 0;
	g_countdown_req               = 0;
	alp_som_pd_store_clear();
	g_quiesce_mode = ALP_POWER_MODE_RUN;

	g_state = (alp_power_backend_state_t){
		.wake_bitmap = ALP_POWER_WAKE_TIMER,
		.retain      = { .level = ALP_POWER_RETAIN_NONE },
	};
}

static void before(void *f)
{
	(void)f;
	reset_fakes();
}

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
static alp_som_bench_knobs_t g_initial_knobs;

static void *suite_setup(void)
{
	g_initial_knobs = alp_som_bench_knobs; /* what the build defaults to, before any test */
	return NULL;
}
#endif

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
ZTEST_SUITE(power_alif_se, NULL, suite_setup, before, NULL, NULL);
#else
ZTEST_SUITE(power_alif_se, NULL, NULL, before, NULL, NULL);
#endif

static void assert_no_side_effect(void)
{
	zassert_equal(g_ev_n, ev_count(EV_GET_OFF_CFG), "only read-only SE getter calls may happen");
	zassert_equal(ev_count(EV_QUIESCE), 0u);
	zassert_equal(ev_count(EV_SET_OFF_CFG), 0u);
	zassert_equal(ev_count(EV_ENTER), 0u);
	zassert_equal(ev_count(EV_RESTORE), 0u);
}

/* ---- Profile construction ----------------------------------------------------- */

/* Every member poisoned, then built: no member may keep the poison. */
static void poison(off_profile_t *p)
{
	memset(p, 0xA5, sizeof(*p));
}

static void assert_all_assigned(const off_profile_t *p)
{
	const uint32_t *w = (const uint32_t *)p;

	for (size_t i = 0; i < sizeof(*p) / sizeof(uint32_t); ++i) {
		zassert_not_equal(w[i], POISON32, "off_profile_t word %u was never assigned", (unsigned)i);
	}
}

ZTEST(power_alif_se, test_off_profile_every_member_explicit)
{
	sleep_plan_t  plan = { .mode          = ALP_POWER_MODE_STOP,
		                   .hw            = ALP_SOM_ARM_LPTIMER,
		                   .memory_blocks = ALP_AIPM_GEN2_BACKUP4K_MASK };
	off_profile_t out;

	poison(&out);
	zassert_equal(build_off_profile(&out, &g_live, &plan), ALP_OK);
	assert_all_assigned(&out);

	zassert_equal(out.power_domains, PD_VBAT_AON_MASK);
	zassert_equal(out.dcdc_voltage, 825u);
	zassert_equal(out.dcdc_mode, DCDC_MODE_OFF);
	zassert_equal(out.aon_clk_src, CLK_SRC_LFRC, "LFXO is not confirmed -> LFRC");
	zassert_equal(out.stby_clk_src, CLK_SRC_HFRC);
	zassert_equal(out.stby_clk_freq, SCALED_FREQ_RC_STDBY_0_075_MHZ);
	zassert_equal(out.ip_clock_gating, 0u, "live 0x1234 must not leak into the profile");
	zassert_equal(out.phy_pwr_gating, 0u, "live 0x1d must not leak into the profile");
	zassert_equal(out.vdd_ioflex_3V3, IOFLEX_LEVEL_1V8);
}

ZTEST(power_alif_se, test_off_profile_standby_differs_only_where_documented)
{
	sleep_plan_t  plan = { .mode          = ALP_POWER_MODE_STANDBY,
		                   .hw            = ALP_SOM_ARM_LPTIMER,
		                   .memory_blocks = ALP_AIPM_GEN2_BACKUP4K_MASK };
	off_profile_t out;

	poison(&out);
	zassert_equal(build_off_profile(&out, &g_live, &plan), ALP_OK);
	assert_all_assigned(&out);
	zassert_equal(out.power_domains, PD_VBAT_AON_MASK | PD_SSE700_AON_MASK);
	zassert_equal(out.stby_clk_freq, SCALED_FREQ_RC_STDBY_76_8_MHZ);
}

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
ZTEST(power_alif_se, test_off_profile_vtor_preserved_from_live)
{
	sleep_plan_t  plan = { .mode          = ALP_POWER_MODE_STOP,
		                   .hw            = ALP_SOM_ARM_LPTIMER,
		                   .memory_blocks = ALP_AIPM_GEN2_BACKUP4K_MASK };
	off_profile_t out;

	poison(&out);
	g_live.vtor_address    = 0u; /* the SE default: boot through the ATOC */
	g_live.vtor_address_ns = 0u;
	zassert_equal(build_off_profile(&out, &g_live, &plan), ALP_OK);
	zassert_equal(out.vtor_address, 0u);
	zassert_equal(out.vtor_address_ns, 0u);

	g_live.vtor_address    = 0x80012000u;
	g_live.vtor_address_ns = 0x80012100u;
	zassert_equal(build_off_profile(&out, &g_live, &plan), ALP_OK);
	zassert_equal(out.vtor_address, 0x80012000u);
	zassert_equal(out.vtor_address_ns, 0x80012100u);
}
#endif

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
ZTEST(power_alif_se, test_off_profile_masks_come_from_the_gen2_header)
{
	sleep_plan_t  plan = { .mode = ALP_POWER_MODE_STOP };
	off_profile_t out;

	/* LPTIMER only */
	plan.hw            = ALP_SOM_ARM_LPTIMER;
	plan.memory_blocks = retained_blocks(&(alp_power_retain_t){ .level = ALP_POWER_RETAIN_NONE });
	poison(&out);
	zassert_ok(build_off_profile(&out, &g_live, &plan));
	zassert_equal(out.memory_blocks, UINT32_C(0x00200000), "BKRAM is gen2 bit 21, not gen1 bit 20");
	zassert_equal(out.memory_blocks, ALP_AIPM_GEN2_BACKUP4K_MASK);
	zassert_equal(out.memory_blocks & ALP_AIPM_GEN2_FWRAM_MASK, 0u, "bit 20 is FWRAM on gen2");
	zassert_equal(out.wakeup_events, ALP_AIPM_GEN2_WE_LPTIMER0);
	zassert_equal(out.wakeup_events, UINT32_C(0x100));
	zassert_equal(out.ewic_cfg, ALP_AIPM_GEN2_EWIC_VBAT_TIMER);

	/* RV-3028 /INT on LPGPIO0 */
	plan.hw = ALP_SOM_ARM_RTC_TIMER;
	zassert_ok(build_off_profile(&out, &g_live, &plan));
	zassert_equal(out.wakeup_events, ALP_AIPM_GEN2_WE_LPGPIO0);
	zassert_equal(out.wakeup_events, UINT32_C(0x10000));
	zassert_equal(out.ewic_cfg, ALP_AIPM_GEN2_EWIC_VBAT_GPIO);

	/* both */
	plan.hw = ALP_SOM_ARM_LPTIMER | ALP_SOM_ARM_RTC_INT;
	zassert_ok(build_off_profile(&out, &g_live, &plan));
	zassert_equal(out.wakeup_events, ALP_AIPM_GEN2_WE_LPTIMER0 | ALP_AIPM_GEN2_WE_LPGPIO0);
	zassert_equal(out.ewic_cfg, ALP_AIPM_GEN2_EWIC_VBAT_TIMER | ALP_AIPM_GEN2_EWIC_VBAT_GPIO);
}
#endif

ZTEST(power_alif_se, test_off_profile_lfxo_only_when_selected)
{
	sleep_plan_t  plan = { .mode          = ALP_POWER_MODE_STOP,
		                   .hw            = ALP_SOM_ARM_LPTIMER,
		                   .memory_blocks = ALP_AIPM_GEN2_BACKUP4K_MASK };
	off_profile_t out;

	plan.lfxo = true;
	poison(&out);
	zassert_ok(build_off_profile(&out, &g_live, &plan));
	zassert_equal(out.aon_clk_src, CLK_SRC_LFXO);

	/* lfxo_confirmed(): needs SEL_32K and XTAL32K_EN together. */
	g_regs[ALIF_SE_REG_ANA_MISC] = 0u;
	g_regs[ALIF_SE_REG_ANA_REG1] = ANA_REG1_XTAL32K_EN;
	zassert_false(lfxo_confirmed(), "the SES boot leaves XTAL32K_EN=1 with LFRC selected");
	g_regs[ALIF_SE_REG_ANA_MISC] = ANA_MISC_SEL_32K;
	g_regs[ALIF_SE_REG_ANA_REG1] = 0u;
	zassert_false(lfxo_confirmed());
	g_regs[ALIF_SE_REG_ANA_REG1] = ANA_REG1_XTAL32K_EN;
	zassert_true(lfxo_confirmed());
}

ZTEST(power_alif_se, test_off_profile_rejects_garbled_live_dcdc)
{
	sleep_plan_t  plan = { .mode = ALP_POWER_MODE_STOP, .hw = ALP_SOM_ARM_LPTIMER };
	off_profile_t out;

	g_live.dcdc_voltage = 0u;
	zassert_equal(build_off_profile(&out, &g_live, &plan), ALP_ERR_IO);
	g_live.dcdc_voltage = 900u;
	zassert_equal(build_off_profile(&out, &g_live, &plan), ALP_ERR_IO);
	g_live.dcdc_voltage = 750u;
	zassert_equal(build_off_profile(&out, &g_live, &plan), ALP_OK);
	g_live.dcdc_voltage = 850u;
	zassert_equal(build_off_profile(&out, &g_live, &plan), ALP_OK);
}

/* ---- Retention --------------------------------------------------------------- */

ZTEST(power_alif_se, test_retention_blocks)
{
	alp_power_retain_t r = { .level = ALP_POWER_RETAIN_NONE };

	zassert_equal(retained_blocks(&r), ALP_AIPM_GEN2_BACKUP4K_MASK);
	r.level = ALP_POWER_RETAIN_UTILITY;
	zassert_equal(retained_blocks(&r), ALP_AIPM_GEN2_BACKUP4K_MASK, "UTILITY == NONE");
	r = (alp_power_retain_t){ .level = ALP_POWER_RETAIN_TCM, .retain_kb = 1u };
	zassert_equal(retained_blocks(&r), ALP_AIPM_GEN2_BACKUP4K_MASK | ALP_AIPM_GEN2_SRAM5_1_MASK);
	r.retain_kb = 128u;
	zassert_equal(retained_blocks(&r), ALP_AIPM_GEN2_BACKUP4K_MASK | ALP_AIPM_GEN2_SRAM5_1_MASK);
	r.retain_kb = 129u; /* rounds UP to the next bank */
	zassert_equal(retained_blocks(&r),
	              ALP_AIPM_GEN2_BACKUP4K_MASK | ALP_AIPM_GEN2_SRAM5_1_MASK |
	                  ALP_AIPM_GEN2_SRAM5_2_MASK);
	r.retain_kb = 512u;
	zassert_equal(retained_blocks(&r),
	              ALP_AIPM_GEN2_BACKUP4K_MASK | ALP_AIPM_GEN2_SRAM5_1_MASK |
	                  ALP_AIPM_GEN2_SRAM5_2_MASK | ALP_AIPM_GEN2_SRAM4_1_MASK |
	                  ALP_AIPM_GEN2_SRAM4_2_MASK);
	r = (alp_power_retain_t){ .level = ALP_POWER_RETAIN_FULL };
	zassert_equal(
	    retained_blocks(&r),
	    retained_blocks(&(alp_power_retain_t){ .level = ALP_POWER_RETAIN_TCM, .retain_kb = 512u }));
}

ZTEST(power_alif_se, test_configure_retention_codes)
{
	alp_power_retain_t r = { .level = ALP_POWER_RETAIN_NONE };

	zassert_equal(se_configure_retention(&g_state, &r), ALP_OK);
	r.level = ALP_POWER_RETAIN_UTILITY;
	zassert_equal(se_configure_retention(&g_state, &r), ALP_OK);
	r = (alp_power_retain_t){ .level = ALP_POWER_RETAIN_TCM, .retain_kb = 0u };
	zassert_equal(se_configure_retention(&g_state, &r), ALP_ERR_INVAL);
	r.retain_kb = 513u;
	zassert_equal(se_configure_retention(&g_state, &r), ALP_ERR_NOSUPPORT);
	r.retain_kb = 512u;
	zassert_equal(se_configure_retention(&g_state, &r), ALP_OK);
	r = (alp_power_retain_t){ .level = (alp_power_retain_level_t)99 };
	zassert_equal(se_configure_retention(&g_state, &r), ALP_ERR_INVAL);
}

/* ---- Wake planning ------------------------------------------------------------ */

ZTEST(power_alif_se, test_short_timed_wake_uses_lptimer)
{
	alp_power_wake_info_t info;

	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, &info), ALP_OK);
	zassert_equal(g_armed_ticks, 18023u, "500 ms, rounded UP at 36045 Hz (LFRC +10 %)");
	zassert_equal(ev_count(EV_ARM_LPTIMER), 1u);
	zassert_equal(ev_count(EV_ARM_RTC_TIMER), 0u);
	zassert_equal(g_rec_at_set.armed_hw & ~ALP_SOM_REC_NSRST_TRUSTED, ALP_SOM_ARM_LPTIMER);
	zassert_equal(g_set.wakeup_events, ALP_AIPM_GEN2_WE_LPTIMER0);
}

ZTEST(power_alif_se, test_timed_wake_boundary_999_and_1000)
{
	alp_power_wake_info_t info;

	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 999u, &info), ALP_OK);
	zassert_equal(g_armed_ticks, 36009u, "999 ms: LPTIMER, rounded up");
	zassert_equal(ev_count(EV_ARM_RTC_TIMER), 0u);

	reset_fakes();
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 1000u, &info), ALP_OK);
	zassert_equal(ev_count(EV_ARM_LPTIMER), 0u, "1000 ms: RV-3028 countdown");
	zassert_equal(g_countdown_req, 1u);
	zassert_equal(g_rec_at_set.armed_hw & ~ALP_SOM_REC_NSRST_TRUSTED, ALP_SOM_ARM_RTC_TIMER);
	zassert_equal(g_set.wakeup_events, ALP_AIPM_GEN2_WE_LPGPIO0);
	zassert_equal(g_rec_at_set.armed,
	              ALP_POWER_WAKE_TIMER,
	              "a TIMER request served by the RV-3028 countdown still reports TIMER");

	reset_fakes();
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 1001u, &info), ALP_OK);
	zassert_equal(g_countdown_req, 2u, "rounded UP to whole seconds");
	zassert_equal(g_rec_at_set.armed_ms, 2000u);
}

ZTEST(power_alif_se, test_long_wake_without_rtc_countdown_is_nosupport)
{
	alp_power_wake_info_t info;

	g_countdown_ready   = false;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 5000u, &info), ALP_ERR_NOSUPPORT);
	assert_no_side_effect();
}

ZTEST(power_alif_se, test_countdown_too_long_is_inval)
{
	alp_power_wake_info_t info;

	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(
	    se_request_sleep(
	        &g_state, ALP_POWER_MODE_STOP, (RV3028C7_TIMER_MAX_SECONDS + 1u) * 1000u, &info),
	    ALP_ERR_INVAL);
	assert_no_side_effect();
}

ZTEST(power_alif_se, test_nothing_to_wake_is_inval)
{
	g_state.wake_bitmap = 0u;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 0u, NULL), ALP_ERR_INVAL);
	assert_no_side_effect();
}

ZTEST(power_alif_se, test_timer_bit_without_length_is_inval)
{
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 0u, NULL), ALP_ERR_INVAL);
	assert_no_side_effect();
}

ZTEST(power_alif_se, test_rtc_wake_needs_the_callers_alarm_to_be_armed)
{
	g_state.wake_bitmap = ALP_POWER_WAKE_RTC;
	g_rtc_int_armed     = false;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 0u, NULL),
	              ALP_ERR_INVAL,
	              "would sleep with nothing to wake it");
	assert_no_side_effect();

	reset_fakes();
	g_state.wake_bitmap = ALP_POWER_WAKE_RTC;
	g_rtc_int_armed     = true;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 0u, NULL), ALP_OK);
	zassert_equal(g_rec_at_set.armed_hw & ~ALP_SOM_REC_NSRST_TRUSTED, ALP_SOM_ARM_RTC_INT);
	zassert_equal(ev_count(EV_ARM_RTC_TIMER), 0u, "the SDK starts no countdown of its own");
	zassert_equal(ev_count(EV_ARM_INT_PAD), 1u);
}

ZTEST(power_alif_se, test_unadvertised_wake_bits_are_nosupport)
{
	g_state.wake_bitmap = ALP_POWER_WAKE_GPIO;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 100u, NULL), ALP_ERR_NOSUPPORT);
	g_state.wake_bitmap = ALP_POWER_WAKE_UART_RX | ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 100u, NULL), ALP_ERR_NOSUPPORT);
	assert_no_side_effect();
}

ZTEST(power_alif_se, test_advertised_wake_caps_are_the_real_ones)
{
	uint32_t caps = 0;

	zassert_ok(se_open(&g_state, NULL, &caps));
	zassert_equal(caps, ALP_POWER_WAKE_RTC | ALP_POWER_WAKE_TIMER);

	g_int_present     = false;
	g_countdown_ready = true;
	zassert_equal(se_mode_wake_caps(&g_state, ALP_POWER_MODE_STOP),
	              ALP_POWER_WAKE_TIMER,
	              "no INT pad: no RTC wake, and the countdown has no way to reach the SoC");

	g_timer_present = false;
	zassert_equal(se_mode_wake_caps(&g_state, ALP_POWER_MODE_STOP), 0u);

	g_int_present = true;
	zassert_equal(se_mode_wake_caps(&g_state, ALP_POWER_MODE_STOP),
	              ALP_POWER_WAKE_RTC | ALP_POWER_WAKE_TIMER,
	              "RV countdown stands in for the timer");
	zassert_equal(se_mode_wake_caps(&g_state, ALP_POWER_MODE_STANDBY),
	              ALP_POWER_WAKE_RTC | ALP_POWER_WAKE_TIMER);
	zassert_equal(se_mode_wake_caps(&g_state, ALP_POWER_MODE_SLEEP),
	              0u,
	              "no pm_policy in this image: SLEEP arms nothing");
}

ZTEST(power_alif_se, test_rtc_domain_rail_off_conflicts_with_rtc_wake)
{
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	g_rtc_policy        = ALP_POWER_DOMAIN_POLICY_RAIL_OFF;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 2000u, NULL), ALP_ERR_INVAL);
	assert_no_side_effect();

	/* An LPTIMER-only wake does not use the RTC, so the policy is no conflict. */
	reset_fakes();
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	g_rtc_policy        = ALP_POWER_DOMAIN_POLICY_RAIL_OFF;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
}

ZTEST(power_alif_se, test_mode_dispatch)
{
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_RUN, 100u, NULL), ALP_ERR_INVAL);
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_SLEEP, 100u, NULL),
	              ALP_ERR_NOSUPPORT,
	              "no pm_policy backend in this image");
	assert_no_side_effect();
}

/* ---- Refusals ------------------------------------------------------------------ */

#ifndef CONFIG_ALP_SDK_POWER_ALIF_SE_ALLOW_DEBUGGER
ZTEST(power_alif_se, test_debugger_attached_is_refused_with_busy)
{
	g_debugger = true;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_ERR_BUSY);
	assert_no_side_effect();
	zassert_equal(ev_count(EV_ARM_LPTIMER), 0u);
	zassert_false(g_rec_valid, "no record written");
}
#else
ZTEST(power_alif_se, test_debugger_override_lets_the_sleep_proceed)
{
	g_debugger = true;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_equal(ev_count(EV_ENTER), 1u);
}
#endif

ZTEST(power_alif_se, test_dcache_and_lpstate_refusals)
{
	g_dcache = true;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_ERR_NOSUPPORT);
	assert_no_side_effect();

	reset_fakes();
	g_lpstate_off = false;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_ERR_NOT_READY);
	assert_no_side_effect();
}

ZTEST(power_alif_se, test_armed_source_already_pending_is_refused_with_busy)
{
	g_timer_pending     = true;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_ERR_BUSY);
	assert_no_side_effect();

	/* A pending LPTIMER is irrelevant when the LPTIMER is not what is armed. */
	reset_fakes();
	g_timer_pending     = true;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_OK);

	/* RV-3028 /INT already low, with an enabled countdown flag behind it. */
	reset_fakes();
	g_int_level         = 1;
	g_rtc_int_armed     = true;  /* the caller's own countdown, not this backend's */
	g_rtc_regs[0]       = 0x08u; /* TF */
	g_rtc_regs[2]       = 0x10u; /* TIE */
	g_state.wake_bitmap = ALP_POWER_WAKE_RTC;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 0u, NULL), ALP_ERR_BUSY);
	assert_no_side_effect();

	/* An unreadable /INT does not count as pending. */
	reset_fakes();
	g_int_level         = -19;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_OK);
}

ZTEST(power_alif_se, test_se_getter_failure_changes_nothing)
{
	g_get_rc = -5;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_ERR_IO);
	assert_no_side_effect();

	reset_fakes();
	g_live.dcdc_voltage = 3000u;
	g_stored            = g_live;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_ERR_IO);
	assert_no_side_effect();
}

/* ---- The sequence and its unwinds ---------------------------------------------- */

ZTEST(power_alif_se, test_happy_path_order)
{
	alp_power_wake_info_t info;

	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, &info), ALP_OK);

	zassert_equal(ev_count(EV_QUIESCE), 1u);
	zassert_equal(g_quiesce_mode, ALP_POWER_MODE_STOP);
	zassert_equal(ev_count(EV_ENTER), 1u);
	zassert_true(ev_pos(EV_QUIESCE) < ev_pos(EV_SAVE_RECORD), "quiesce, then record");
	zassert_true(ev_pos(EV_SAVE_RECORD) < ev_pos(EV_SET_OFF_CFG), "record before the SE call");
	/* The LPTIMER is armed LAST: after the SE write and readback, inside the entry. */
	zassert_true(ev_pos(EV_SET_OFF_CFG) < ev_pos(EV_ARM_LPTIMER), "timer armed after the SE calls");
	zassert_true(ev_pos(EV_GET_OFF_CFG) < ev_pos(EV_ARM_LPTIMER));
	zassert_true(ev_pos(EV_ARM_LPTIMER) < ev_pos(EV_ENTER), "and immediately before the WFI");
	zassert_equal(g_enter_lptimer_ticks, g_armed_ticks);

	/* The fake enter returned: an aborted sleep, reported as an early RUN return. */
	zassert_equal(info.realised_mode, ALP_POWER_MODE_RUN);
	zassert_equal(ev_count(EV_DISARM_LPTIMER), 1u);
	zassert_equal(ev_count(EV_RESTORE), 1u);
	zassert_false(g_rec_valid, "record cleared on the aborted path");
}

ZTEST(power_alif_se, test_record_carries_the_cycle)
{
	g_rtc_seconds       = 777u;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STANDBY, 1500u, NULL), ALP_OK);
	zassert_equal(g_set.power_domains, PD_VBAT_AON_MASK | PD_SSE700_AON_MASK);
	zassert_equal(g_quiesce_mode, ALP_POWER_MODE_STANDBY);

	/* The record as the SE call saw it: this is what the cold-boot wake decodes. */
	zassert_equal(g_rec_at_set.mode, (uint32_t)ALP_POWER_MODE_STANDBY);
	zassert_equal(g_rec_at_set.armed, ALP_POWER_WAKE_TIMER);
	zassert_equal(g_rec_at_set.timed_bit, ALP_POWER_WAKE_TIMER);
	zassert_equal(g_rec_at_set.armed_hw & ~ALP_SOM_REC_NSRST_TRUSTED, ALP_SOM_ARM_RTC_TIMER);
	zassert_equal(g_rec_at_set.armed_ms, 2000u);
	zassert_equal(g_rec_at_set.entry_rtc_s, 777u);
	zassert_equal(g_rec_at_set.wake_source, 0u);
	zassert_equal(g_rec_at_set.slept_ms, 0u);

	/* An unreadable RV-3028 leaves the entry time unknown, not zero-by-accident. */
	reset_fakes();
	g_rtc_seconds_ok = false;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_equal(g_rec_at_set.entry_rtc_s, 0u);
	zassert_equal(g_rec_at_set.armed_hw & ~ALP_SOM_REC_NSRST_TRUSTED, ALP_SOM_ARM_LPTIMER);
}

ZTEST(power_alif_se, test_quiesce_failure_arms_nothing)
{
	g_quiesce_rc = ALP_ERR_IO;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_ERR_IO);
	zassert_equal(ev_count(EV_QUIESCE), 1u);
	zassert_equal(ev_count(EV_ARM_LPTIMER), 0u);
	zassert_equal(ev_count(EV_SET_OFF_CFG), 0u);
	zassert_equal(ev_count(EV_ENTER), 0u);
	zassert_equal(ev_count(EV_RESTORE), 0u, "quiesce already rolled itself back");
}

static void assert_unwound(void)
{
	zassert_equal(ev_count(EV_ENTER), 0u, "never enter after a failure");
	zassert_equal(ev_count(EV_RESTORE), 1u, "domains restored");
	zassert_false(g_rec_valid, "record invalidated");
}

ZTEST(power_alif_se, test_set_off_cfg_failure_unwinds)
{
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	g_set_rc            = -5;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_ERR_IO);
	assert_unwound();
	zassert_equal(ev_count(EV_CANCEL_RTC_TIMER), 1u);
	zassert_equal(ev_count(EV_DISARM_INT_PAD), 1u);
	zassert_true(ev_pos(EV_DISARM_INT_PAD) < ev_pos(EV_RESTORE), "disarm before restore");
}

ZTEST(power_alif_se, test_se_dropping_a_requested_bit_unwinds)
{
	/* The SE refused to set memory_blocks bit 20 back on the bench; a silently
	 * dropped retention bit must stop the sleep. */
	g_se_drops_memory_bits = ALP_AIPM_GEN2_BACKUP4K_MASK;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_ERR_IO);
	assert_unwound();
	zassert_equal(ev_count(EV_DISARM_LPTIMER), 1u);
}

ZTEST(power_alif_se, test_arm_failure_unwinds_in_reverse)
{
	/* The timer is armed inside the entry, after the SE write: a failure there undoes it. */
	g_arm_timer_rc = ALP_ERR_IO;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_ERR_IO);
	zassert_equal(ev_count(EV_ENTER), 0u);
	zassert_equal(ev_count(EV_RESTORE), 1u);
	zassert_equal(ev_count(EV_SET_OFF_CFG), 2u, "profile written, then written back");

	reset_fakes();
	g_arm_int_rc        = ALP_ERR_IO;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_ERR_IO);
	assert_unwound();
	zassert_equal(ev_count(EV_CANCEL_RTC_TIMER), 1u, "the countdown already started is stopped");
	zassert_equal(ev_count(EV_SET_OFF_CFG), 0u);
}

ZTEST(power_alif_se, test_countdown_start_failure_unwinds)
{
	g_countdown_rc      = ALP_ERR_IO;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_ERR_IO);
	assert_unwound();
	zassert_equal(ev_count(EV_CANCEL_RTC_TIMER), 0u, "nothing to cancel: it never started");
}

/* ---- Retention registers: re-asserted and verified ------------------------------- */

ZTEST(power_alif_se, test_se_side_effect_is_reasserted)
{
	/* The state the bench saw after a set_run_cfg read-modify-write: retention LDOs
	 * off, CVM masks cleared. */
	g_regs[ALIF_SE_REG_RET_CTRL] = 0x0002AAF1u & ~RET_CTRL_BKRAM;
	g_regs[ALIF_SE_REG_ANA_REG1] = 0x06441f40u & ~ANA_REG1_RET_LDO_VBAT_EN;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_true(ev_count(EV_REG_WRITE) >= 2u);
	zassert_true((g_regs_at_enter[ALIF_SE_REG_RET_CTRL] & RET_CTRL_BKRAM) != 0u);
	zassert_true((g_regs_at_enter[ALIF_SE_REG_ANA_REG1] & ANA_REG1_RET_LDO_VBAT_EN) != 0u);
	/* nothing the SE left set is cleared */
	zassert_equal(g_regs_at_enter[ALIF_SE_REG_RET_CTRL] & 0x0002AAF0u, 0x0002AAF0u);
}

ZTEST(power_alif_se, test_untouched_registers_are_not_written)
{
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_equal(ev_count(EV_REG_WRITE), 0u, "already right: no write");
}

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
ZTEST(power_alif_se, test_tcm_retention_needs_ldo2_and_tcm_masks)
{
	g_state.retain = (alp_power_retain_t){ .level = ALP_POWER_RETAIN_TCM, .retain_kb = 64u };
	g_regs[ALIF_SE_REG_RET_CTRL] = RET_CTRL_BKRAM;
	g_regs[ALIF_SE_REG_ANA_REG1] = ANA_REG1_RET_LDO_VBAT_EN;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_equal(g_set.memory_blocks, ALP_AIPM_GEN2_BACKUP4K_MASK | ALP_AIPM_GEN2_SRAM5_1_MASK);
	zassert_true((g_regs_at_enter[ALIF_SE_REG_ANA_REG1] & ANA_REG1_RET_LDO_VDDMAIN_EN) != 0u);
	zassert_true((g_regs_at_enter[ALIF_SE_REG_RET_CTRL] & (RET_CTRL_HETCM1 | RET_CTRL_HETCM2)) ==
	             (RET_CTRL_HETCM1 | RET_CTRL_HETCM2));
}
#endif

ZTEST(power_alif_se, test_retention_that_does_not_stick_refuses_to_sleep)
{
	g_regs[ALIF_SE_REG_RET_CTRL]           = 0u;
	g_reg_stuck_mask[ALIF_SE_REG_RET_CTRL] = RET_CTRL_BKRAM; /* write cannot set it */
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_ERR_IO);
	assert_unwound();
}

ZTEST(power_alif_se, test_lfxo_trim_is_set_to_63_when_selected)
{
	g_regs[ALIF_SE_REG_ANA_MISC] = ANA_MISC_SEL_32K;
	g_regs[ALIF_SE_REG_ANA_REG1] = ANA_REG1_XTAL32K_EN | ANA_REG1_RET_LDO_VBAT_EN |
	                               (8u << ALP_ALIF_SE_SVD_ANA_REG1_XTAL32K_CAP_CONT_LSB);
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_equal(g_set.aon_clk_src, CLK_SRC_LFXO);
	zassert_equal((g_regs_at_enter[ALIF_SE_REG_ANA_REG1] & ANA_REG1_CAP_CONT_MASK) >>
	                  ALP_ALIF_SE_SVD_ANA_REG1_XTAL32K_CAP_CONT_LSB,
	              63u);
	zassert_true((g_regs_at_enter[ALIF_SE_REG_ANA_REG1] & ANA_REG1_XTAL32K_EN) != 0u);
}

ZTEST(power_alif_se, test_lfrc_leaves_the_trim_alone)
{
	g_regs[ALIF_SE_REG_ANA_REG1] = 0x06441f40u;
	uint32_t before              = g_regs[ALIF_SE_REG_ANA_REG1];

	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_equal(g_set.aon_clk_src, CLK_SRC_LFRC);
	zassert_equal(g_regs[ALIF_SE_REG_ANA_REG1], before);
}

/* ---- Wake decode ----------------------------------------------------------------- */

ZTEST(power_alif_se, test_decode_early_lptimer)
{
	alp_som_pd_record_t rec = { .armed_hw  = ALP_SOM_ARM_LPTIMER,
		                        .armed_ms  = 500u,
		                        .timed_bit = ALP_POWER_WAKE_TIMER };

	g_timer_pending = false;
	alp_som_power_wake_decode_early(&rec);
	zassert_equal(rec.wake_source, 0u, "unlatched: no claim");

	g_timer_pending = true;
	alp_som_power_wake_decode_early(&rec);
	zassert_equal(rec.wake_source, ALP_POWER_WAKE_TIMER);

	/* a latched LPTIMER that this cycle never armed proves nothing */
	rec = (alp_som_pd_record_t){ .armed_hw = ALP_SOM_ARM_RTC_TIMER };
	alp_som_power_wake_decode_early(&rec);
	zassert_equal(rec.wake_source, 0u);
}

ZTEST(power_alif_se, test_decode_i2c_rtc_flags_and_slept_time)
{
	alp_som_pd_record_t rec = { .armed_hw    = ALP_SOM_ARM_RTC_TIMER,
		                        .armed_ms    = 2000u,
		                        .timed_bit   = ALP_POWER_WAKE_RTC,
		                        .entry_rtc_s = 1000u };

	g_rtc_seconds = 1003u;
	g_rtc_flags   = RV3028C7_WAKE_TF;
	alp_som_power_wake_decode_i2c(&rec);
	zassert_equal(rec.wake_source, ALP_POWER_WAKE_RTC);
	zassert_equal(rec.slept_ms, 3000u, "calendar delta, not the nominal length");

	rec           = (alp_som_pd_record_t){ .armed_hw = ALP_SOM_ARM_RTC_INT, .entry_rtc_s = 50u };
	g_rtc_seconds = 51u;
	g_rtc_flags   = RV3028C7_WAKE_AF;
	alp_som_power_wake_decode_i2c(&rec);
	zassert_equal(rec.wake_source, ALP_POWER_WAKE_RTC);
	zassert_equal(rec.slept_ms, 1000u);
}

ZTEST(power_alif_se, test_decode_i2c_no_flags_means_unknown_cause)
{
	alp_som_pd_record_t rec = { .armed_hw    = ALP_SOM_ARM_RTC_TIMER,
		                        .timed_bit   = ALP_POWER_WAKE_RTC,
		                        .entry_rtc_s = 10u };

	g_rtc_seconds = 12u;
	g_rtc_flags   = 0u;
	alp_som_power_wake_decode_i2c(&rec);
	zassert_equal(rec.wake_source, 0u, "no latched flag: do not invent a cause");
	zassert_equal(rec.slept_ms, 2000u, "the time is still known");
}

ZTEST(power_alif_se, test_decode_i2c_lptimer_wake_falls_back_to_nominal_length)
{
	alp_som_pd_record_t rec = { .armed_hw    = ALP_SOM_ARM_LPTIMER,
		                        .armed_ms    = 500u,
		                        .wake_source = ALP_POWER_WAKE_TIMER,
		                        .timed_bit   = ALP_POWER_WAKE_TIMER,
		                        .entry_rtc_s = 0u };

	g_rtc_seconds_ok = false;
	alp_som_power_wake_decode_i2c(&rec);
	zassert_equal(rec.wake_source, ALP_POWER_WAKE_TIMER);
	zassert_equal(rec.slept_ms, 500u);

	/* with a readable RV-3028 the calendar delta wins */
	rec              = (alp_som_pd_record_t){ .armed_hw    = ALP_SOM_ARM_LPTIMER,
		                                      .armed_ms    = 500u,
		                                      .wake_source = ALP_POWER_WAKE_TIMER,
		                                      .entry_rtc_s = 100u };
	g_rtc_seconds_ok = true;
	g_rtc_seconds    = 101u;
	alp_som_power_wake_decode_i2c(&rec);
	zassert_equal(rec.slept_ms, 1000u);
}

ZTEST(power_alif_se, test_decode_i2c_clock_going_backwards_is_not_a_duration)
{
	alp_som_pd_record_t rec = { .armed_hw    = ALP_SOM_ARM_LPTIMER,
		                        .armed_ms    = 500u,
		                        .wake_source = ALP_POWER_WAKE_TIMER,
		                        .timed_bit   = ALP_POWER_WAKE_TIMER,
		                        .entry_rtc_s = 500u };

	g_rtc_seconds = 100u; /* the RV-3028 was set back mid-sleep */
	alp_som_power_wake_decode_i2c(&rec);
	zassert_equal(rec.slept_ms, 500u, "falls back to the nominal length");
}

ZTEST(power_alif_se, test_aborted_sleep_reports_the_source_that_fired)
{
	alp_power_wake_info_t info;

	/* LPTIMER fired: reported as the requested TIMER, an ordinary early wake. */
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, &info), ALP_OK);
	zassert_equal(info.wake_source, ALP_POWER_WAKE_TIMER);
	zassert_equal(info.realised_mode, ALP_POWER_MODE_RUN, "it never reached STOP");

	/* A TIMER request served by the RV-3028 countdown still says TIMER. */
	reset_fakes();
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, &info), ALP_OK);
	zassert_equal(info.wake_source, ALP_POWER_WAKE_TIMER);

	/* An RTC request served by the countdown says RTC. */
	reset_fakes();
	g_state.wake_bitmap = ALP_POWER_WAKE_RTC;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, &info), ALP_OK);
	zassert_equal(info.wake_source, ALP_POWER_WAKE_RTC);
}

ZTEST(power_alif_se, test_stayed_up_with_no_source_is_a_failure_with_everything_put_back)
{
	alp_power_wake_info_t info;

	g_enter_fires       = false;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, &info), ALP_ERR_IO);
	zassert_equal(info.realised_mode, ALP_POWER_MODE_RUN);
	zassert_equal(info.wake_source, 0u);
	zassert_equal(ev_count(EV_RESTORE), 1u);
	zassert_equal(ev_count(EV_SET_OFF_CFG), 2u, "profile written, then the live one written back");
	zassert_equal(g_undo_set.memory_blocks, g_live.memory_blocks);
}

/* ---- Review fixes: undo, full readback, never-early timers, PORF, entry arg ------ */

ZTEST(power_alif_se, test_failure_after_the_se_call_writes_everything_back)
{
	/* RET_CTRL lacks BKRAM (so the backend writes it), VBAT_ANA_REG1 cannot take the
	 * LDO-0 enable (so the verification fails afterwards). */
	g_regs[ALIF_SE_REG_RET_CTRL]           = 0x0002AAF0u;
	g_regs[ALIF_SE_REG_ANA_REG1]           = 0x06441f40u & ~ANA_REG1_RET_LDO_VBAT_EN;
	g_reg_stuck_mask[ALIF_SE_REG_ANA_REG1] = ANA_REG1_RET_LDO_VBAT_EN;
	uint32_t ret0                          = g_regs[ALIF_SE_REG_RET_CTRL];
	uint32_t ana0                          = g_regs[ALIF_SE_REG_ANA_REG1];

	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_ERR_IO);
	assert_unwound();
	zassert_equal(g_regs[ALIF_SE_REG_RET_CTRL], ret0, "RET_CTRL snapshot written back");
	zassert_equal(g_regs[ALIF_SE_REG_ANA_REG1], ana0, "VBAT_ANA_REG1 snapshot written back");
	zassert_equal(ev_count(EV_SET_OFF_CFG), 2u, "profile, then the live profile again");
	zassert_equal(g_undo_set.memory_blocks, g_live.memory_blocks);
	zassert_equal(g_undo_set.vtor_address, g_live.vtor_address);
	zassert_equal(g_stored.dcdc_voltage, g_live.dcdc_voltage);
}

ZTEST(power_alif_se, test_a_failed_undo_is_reported_as_io)
{
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	g_set_rc            = -5; /* the SE refuses both the profile and the write-back */
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_ERR_IO);
	zassert_equal(ev_count(EV_ENTER), 0u);
}

ZTEST(power_alif_se, test_a_refusal_before_the_se_call_never_writes_the_se)
{
	g_dcache = true;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_ERR_NOSUPPORT);
	zassert_equal(ev_count(EV_SET_OFF_CFG), 0u);
	g_dcache            = false;
	g_arm_int_rc        = ALP_ERR_IO; /* the RV-3028 countdown path: INT pad refused */
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	g_countdown_rc      = ALP_ERR_IO; /* the countdown itself cannot start (step 5) */
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_ERR_IO);
	zassert_equal(ev_count(EV_SET_OFF_CFG), 0u, "a failure before step 6 has nothing to undo");
}

ZTEST(power_alif_se, test_every_off_profile_member_is_compared)
{
	off_profile_t want = g_live;

	zassert_true(off_profile_matches(&want, &want));
	for (size_t i = 0; i < sizeof(want) / sizeof(uint32_t); ++i) {
		off_profile_t got = want;

		((uint32_t *)&got)[i] ^= 1u;
		zassert_false(off_profile_matches(&want, &got), "member %u is not compared", (unsigned)i);
	}
}

ZTEST(power_alif_se, test_se_rewriting_the_io_rail_or_dcdc_abandons_the_sleep)
{
	/* The SE hands back a profile whose I/O flex rail or DC-DC voltage differs from
	 * what was written: a hardware hazard, never a sleep. */
	g_readback_overridden = true;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL),
	              ALP_ERR_IO,
	              "read back differs from what the fake SE was given");

	reset_fakes();
	g_stored.vdd_ioflex_3V3   = IOFLEX_LEVEL_3V3;
	g_live                    = g_stored;
	g_readback                = g_stored;
	g_readback.vdd_ioflex_3V3 = IOFLEX_LEVEL_3V3;
	g_readback.dcdc_voltage   = 850u;
	g_readback_overridden     = true;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_ERR_IO);
	zassert_equal(ev_count(EV_ENTER), 0u);
}

ZTEST(power_alif_se, test_lptimer_ticks_are_never_early)
{
	/* ceil(ms * fastest_rate / 1000) at every length, both clocks. */
	for (uint32_t ms = 1u; ms < ALIF_SE_LPTIMER_MAX_MS; ++ms) {
		reset_fakes();
		g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
		zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, ms, NULL), ALP_OK);
		zassert_true((uint64_t)g_armed_ticks * 1000u >= (uint64_t)ms * ALIF_SE_LFRC_HZ_MAX,
		             "LFRC %u ms is early",
		             (unsigned)ms);
		zassert_true((uint64_t)(g_armed_ticks - 1u) * 1000u < (uint64_t)ms * ALIF_SE_LFRC_HZ_MAX,
		             "LFRC %u ms is more than one tick late",
		             (unsigned)ms);
	}
	for (uint32_t ms = 1u; ms < ALIF_SE_LPTIMER_MAX_MS; ms += 7u) {
		reset_fakes();
		g_regs[ALIF_SE_REG_ANA_MISC] = ANA_MISC_SEL_32K;
		g_regs[ALIF_SE_REG_ANA_REG1] |= ANA_REG1_XTAL32K_EN;
		g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
		zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, ms, NULL), ALP_OK);
		zassert_true((uint64_t)g_armed_ticks * 1000u >= (uint64_t)ms * ALIF_SE_LFXO_HZ_MAX,
		             "LFXO %u ms is early",
		             (unsigned)ms);
	}
}

ZTEST(power_alif_se, test_lptimer_tick_boundaries)
{
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 1u, NULL), ALP_OK);
	zassert_equal(g_armed_ticks, 37u, "1 ms: ceil(36.045)");

	reset_fakes();
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 999u, NULL), ALP_OK);
	zassert_equal(g_armed_ticks, 36009u);

	reset_fakes();
	g_regs[ALIF_SE_REG_ANA_MISC] = ANA_MISC_SEL_32K;
	g_regs[ALIF_SE_REG_ANA_REG1] |= ANA_REG1_XTAL32K_EN;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_equal(g_armed_ticks, 16388u, "LFXO 500 ms: ceil(16387.5)");
}

ZTEST(power_alif_se, test_rtc_countdown_seconds_never_early)
{
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	for (uint32_t ms = 1000u; ms <= 4200u; ms += 199u) {
		reset_fakes();
		g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
		zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, ms, NULL), ALP_OK);
		zassert_true(g_countdown_req * 1000u >= ms, "%u ms countdown is early", (unsigned)ms);
		zassert_true(g_countdown_req * 1000u < ms + 1000u);
	}
}

ZTEST(power_alif_se, test_entry_gets_the_pad_flag_only_for_rtc_wakes)
{
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_false(g_enter_rtc_int, "LPTIMER wake: the INT pad stays off");
	zassert_equal(ev_count(EV_ARM_INT_PAD), 0u);

	reset_fakes();
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_OK);
	zassert_true(g_enter_rtc_int);
}

/* ---- LPGPIO wake pads (alp,power-wake-gpios) ---------------------------------- */

#define PAD2 BIT(2)
#define PAD3 BIT(3)

ZTEST(power_alif_se, test_gpio_wake_is_advertised_only_for_free_named_pads)
{
	uint32_t caps = 0;

	zassert_ok(se_open(&g_state, NULL, &caps));
	zassert_equal(caps & ALP_POWER_WAKE_GPIO, 0u, "no node: not advertised");

	g_wake_pad_mask = PAD2;
	g_claimed       = 0x03u | PAD3; /* the SoM wires these, not P15_2 here */
	zassert_ok(se_open(&g_state, NULL, &caps));
	zassert_not_equal(caps & ALP_POWER_WAKE_GPIO, 0u);
	zassert_not_equal(se_mode_wake_caps(&g_state, ALP_POWER_MODE_STANDBY) & ALP_POWER_WAKE_GPIO,
	                  0u);

	g_claimed = PAD2;
	zassert_ok(se_open(&g_state, NULL, &caps));
	zassert_equal(caps & ALP_POWER_WAKE_GPIO, 0u, "a SoM-wired pad is refused");
}

ZTEST(power_alif_se, test_a_somwired_pad_refuses_the_whole_set)
{
	g_wake_pad_mask     = PAD2 | PAD3;
	g_claimed           = PAD3;
	g_state.wake_bitmap = ALP_POWER_WAKE_GPIO;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 0u, NULL), ALP_ERR_NOSUPPORT);
	assert_no_side_effect();
}

ZTEST(power_alif_se, test_gpio_wake_builds_the_wake_event_per_line)
{
	g_wake_pad_mask     = PAD2 | BIT(5);
	g_state.wake_bitmap = ALP_POWER_WAKE_GPIO;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 0u, NULL), ALP_OK);
	zassert_equal(g_set.wakeup_events,
	              ALP_AIPM_GEN2_WE_LPGPIO2 | ALP_AIPM_GEN2_WE_LPGPIO5,
	              "line n is WE_LPGPIO<n>, bit 16 + n");
	zassert_equal(g_set.ewic_cfg, ALP_AIPM_GEN2_EWIC_VBAT_GPIO);
	zassert_equal(g_enter_pads, PAD2 | BIT(5));
	zassert_false(g_enter_rtc_int, "the RV-3028 pad stays off for a pad-only wake");
	zassert_equal(g_enter_lptimer_ticks, 0u);
}

ZTEST(power_alif_se, test_gpio_wake_joins_a_timed_wake)
{
	g_wake_pad_mask     = PAD2;
	g_state.wake_bitmap = ALP_POWER_WAKE_GPIO;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_equal(g_set.wakeup_events, ALP_AIPM_GEN2_WE_LPTIMER0 | ALP_AIPM_GEN2_WE_LPGPIO2);
	zassert_equal(g_set.ewic_cfg, ALP_AIPM_GEN2_EWIC_VBAT_TIMER | ALP_AIPM_GEN2_EWIC_VBAT_GPIO);
}

ZTEST(power_alif_se, test_the_record_carries_the_armed_pads)
{
	g_wake_pad_mask     = PAD2;
	g_state.wake_bitmap = ALP_POWER_WAKE_GPIO;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 0u, NULL), ALP_OK);
	zassert_not_equal(g_rec_at_set.armed_hw & ALP_SOM_ARM_LPGPIO, 0u);
	zassert_equal((g_rec_at_set.armed_hw >> ALP_SOM_ARM_PADS_SHIFT) & ALP_SOM_PADS_MASK, PAD2);
	zassert_not_equal(g_rec_at_set.armed & ALP_POWER_WAKE_GPIO, 0u);
}

ZTEST(power_alif_se, test_pad_arm_failure_unwinds_before_the_se_call)
{
	g_wake_pad_mask     = PAD2;
	g_state.wake_bitmap = ALP_POWER_WAKE_GPIO;
	g_arm_pads_rc       = ALP_ERR_IO;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 0u, NULL), ALP_ERR_IO);
	zassert_equal(g_enter_count, 0u);
	zassert_equal(g_set_calls, 0u, "the SE is never written");
	zassert_equal(ev_count(EV_RESTORE), 1u, "the domains are put back");
}

ZTEST(power_alif_se, test_an_already_asserted_pad_is_refused_and_everything_put_back)
{
	g_wake_pad_mask     = PAD2;
	g_state.wake_bitmap = ALP_POWER_WAKE_GPIO;
	g_enter_rc          = ALP_ERR_BUSY; /* the entry saw the pad at its asserted level */
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 0u, NULL), ALP_ERR_BUSY);
	zassert_equal(ev_count(EV_DISARM_WAKE_PADS), 1u);
	zassert_equal(ev_count(EV_RESTORE), 1u);
	zassert_equal(g_set_calls, 2u, "the profile is written, then written back");
}

ZTEST(power_alif_se, test_an_aborted_sleep_reports_gpio_and_consumes_the_edge)
{
	alp_power_wake_info_t info;

	g_wake_pad_mask     = PAD2;
	g_state.wake_bitmap = ALP_POWER_WAKE_GPIO;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 0u, &info), ALP_OK);
	zassert_equal(info.wake_source, ALP_POWER_WAKE_GPIO);
	zassert_equal(info.realised_mode, ALP_POWER_MODE_RUN);
	zassert_equal(g_released_pads, PAD2, "the latched edge is acknowledged");
	zassert_equal(g_pads_latched, 0u);
}

ZTEST(power_alif_se, test_decode_early_reads_the_boot_snapshot_not_the_live_register)
{
	alp_som_pd_record_t rec = {
		.armed_hw = ALP_SOM_ARM_LPGPIO | ((PAD2 | PAD3) << ALP_SOM_ARM_PADS_SHIFT),
	};

	/* gpio_dw's init has already cleared the live register (g_pads_latched == 0); only the
	 * PRE_KERNEL_1 snapshot still holds the edge. */
	g_pads_latched      = 0u;
	g_pads_boot_latched = PAD3;
	alp_som_power_wake_decode_early(&rec);
	zassert_equal(rec.wake_source, ALP_POWER_WAKE_GPIO);

	/* and the other way round: a live edge that the snapshot never saw is not a boot wake */
	rec = (alp_som_pd_record_t){
		.armed_hw = ALP_SOM_ARM_LPGPIO | (PAD2 << ALP_SOM_ARM_PADS_SHIFT),
	};
	g_pads_latched      = PAD2;
	g_pads_boot_latched = 0u;
	alp_som_power_wake_decode_early(&rec);
	zassert_equal(rec.wake_source, 0u);

	/* a snapshot edge on a pad this cycle never armed proves nothing */
	g_pads_boot_latched = PAD3;
	rec                 = (alp_som_pd_record_t){
		                .armed_hw = ALP_SOM_ARM_LPGPIO | (PAD2 << ALP_SOM_ARM_PADS_SHIFT),
	};
	alp_som_power_wake_decode_early(&rec);
	zassert_equal(rec.wake_source, 0u);

	/* a cycle that armed no pad never reports GPIO */
	rec = (alp_som_pd_record_t){ .armed_hw = ALP_SOM_ARM_LPTIMER };
	alp_som_power_wake_decode_early(&rec);
	zassert_equal(rec.wake_source, 0u);
}

ZTEST(power_alif_se, test_entry_failure_unwinds)
{
	g_enter_rc          = ALP_ERR_IO;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_ERR_IO);
	zassert_equal(ev_count(EV_RESTORE), 1u);
	zassert_equal(ev_count(EV_SET_OFF_CFG), 2u);
	zassert_equal(ev_count(EV_CANCEL_RTC_TIMER), 1u);
}

ZTEST(power_alif_se, test_stale_standby_record_is_discarded_after_rtc_power_loss)
{
	alp_som_pd_record_t rec = { .mode      = (uint32_t)ALP_POWER_MODE_STANDBY,
		                        .armed_hw  = ALP_SOM_ARM_LPTIMER,
		                        .timed_bit = ALP_POWER_WAKE_TIMER };

	g_porf = true;
	zassert_false(alp_som_power_wake_decode_i2c(&rec), "RV-3028 lost power: not this cycle");

	g_porf = false;
	zassert_true(alp_som_power_wake_decode_i2c(&rec));

	/* STOP records are vouched for by STOP_MODE_STAT; PORF does not discard them. */
	rec.mode = (uint32_t)ALP_POWER_MODE_STOP;
	g_porf   = true;
	zassert_true(alp_som_power_wake_decode_i2c(&rec));
}

ZTEST(power_alif_se, test_rtc_wake_already_spent_before_entry_is_refused)
{
	/* The countdown started at step 5 fired before the edge interrupt existed: the
	 * flag is latched.  Sleeping on it would never wake. */
	g_flags_pending     = true;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_ERR_BUSY);
	assert_unwound();
	zassert_equal(ev_count(EV_CANCEL_RTC_TIMER), 1u);
	zassert_equal(ev_count(EV_SET_OFF_CFG), 2u, "the profile was written, so it is written back");

	/* An LPTIMER-only wake never looks at the RV-3028 flags. */
	reset_fakes();
	g_flags_pending     = true;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
}

ZTEST(power_alif_se, test_entry_refusing_an_already_asserted_int_unwinds)
{
	/* The entry re-reads /INT with the edge armed (hw seam) and answers BUSY. */
	g_enter_rc          = ALP_ERR_BUSY;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_ERR_BUSY);
	zassert_equal(ev_count(EV_ENTER), 1u, "the entry itself refused");
	zassert_equal(ev_count(EV_RESTORE), 1u);
	zassert_false(g_rec_valid);
	zassert_equal(ev_count(EV_DISARM_INT_PAD), 1u);
	zassert_equal(ev_count(EV_CANCEL_RTC_TIMER), 1u);
}

/* ---- hal_alif does not transport dcdc_mode for the OFF profile -------------------- */

ZTEST(power_alif_se, test_dcdc_mode_sentinel_survives_the_readback)
{
	off_profile_t got;
	off_profile_t want = g_live;

	/* Poison the stack words first: the helper must initialise everything itself. */
	memset(&got, 0xCC, sizeof(got));
	zassert_equal(off_cfg_read(&got), 0);
	zassert_equal(got.dcdc_mode, OFF_DCDC_MODE_SENTINEL, "the client did not fill it");
	zassert_equal(got.power_domains, g_stored.power_domains);
	zassert_equal(got.vtor_address_ns, g_stored.vtor_address_ns);

	want.dcdc_mode = DCDC_MODE_OFF;
	zassert_true(off_profile_matches(&want, &got), "a non-transported member is skipped");
}

ZTEST(power_alif_se, test_the_sleep_proceeds_with_dcdc_mode_not_transported)
{
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_equal(g_set.dcdc_mode, DCDC_MODE_OFF, "OFF is still what is handed to the client");
	zassert_equal(ev_count(EV_ENTER), 1u);
}

ZTEST(power_alif_se, test_a_transported_dcdc_mode_is_compared_strictly)
{
	/* A future hal_alif that carries the member: equal passes. */
	g_transports_dcdc_mode = true;
	g_state.wake_bitmap    = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);

	/* ...and a different value refuses, including 0x7f, the stack garbage the bench saw:
	 * no value is excused. */
	reset_fakes();
	g_transports_dcdc_mode = true;
	g_readback             = g_stored;
	g_readback.dcdc_mode   = (dcdc_mode_t)0x7fu;
	g_readback_overridden  = true;
	g_state.wake_bitmap    = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_ERR_IO);
	zassert_equal(ev_count(EV_ENTER), 0u);
}

ZTEST(power_alif_se, test_each_strict_member_refuses_at_step_6)
{
	/* 13 members stay strict; word 2 is dcdc_mode, which is probed, not compared. */
	for (int i = 0; i < (int)(sizeof(off_profile_t) / sizeof(uint32_t)); ++i) {
		if (i == 2) {
			continue;
		}
		reset_fakes();
		g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
		g_flip_word         = i;
		zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL),
		              ALP_ERR_IO,
		              "member word %d was not compared",
		              i);
		zassert_equal(ev_count(EV_ENTER), 0u, "word %d", i);
	}
}

ZTEST(power_alif_se, test_undo_succeeds_with_the_dcdc_mode_sentinel)
{
	undo_t u;

	memset(&u, 0, sizeof(u));
	u.live           = g_live;
	u.live.dcdc_mode = DCDC_MODE_OFF;
	u.ret_ctrl       = g_regs[ALIF_SE_REG_RET_CTRL];
	u.ana_reg1       = g_regs[ALIF_SE_REG_ANA_REG1];
	g_stored         = g_live;
	zassert_equal(undo_se(&u), ALP_OK);
	zassert_equal(ev_count(EV_SET_OFF_CFG), 1u);

	/* A real mismatch in a strict member still fails the undo. */
	g_readback = g_stored;
	g_readback.ewic_cfg ^= 1u;
	g_readback_overridden = true;
	zassert_equal(undo_se(&u), ALP_ERR_IO);
}

/* ---- LPTIMER armed last, and proven live (bench U8c) ------------------------------ */

ZTEST(power_alif_se, test_lptimer_that_already_fired_is_refused_never_slept_on)
{
	alp_power_wake_info_t info = { .wake_source = 0xFFu };

	g_lptimer_fired_at_arm = true;
	g_state.wake_bitmap    = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, &info), ALP_ERR_BUSY);
	zassert_equal(info.wake_source, 0u, "a refused sleep reports no wake source");
	zassert_equal(info.realised_mode, ALP_POWER_MODE_RUN);
	zassert_equal(ev_count(EV_ENTER), 0u, "no WFI with the wake already spent");
	zassert_equal(ev_count(EV_RESTORE), 1u);
	zassert_equal(ev_count(EV_SET_OFF_CFG), 2u);
	zassert_equal(ev_count(EV_DISARM_LPTIMER), 1u);
}

ZTEST(power_alif_se, test_the_entry_gets_no_ticks_for_an_rtc_only_wake)
{
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_OK);
	zassert_equal(g_enter_lptimer_ticks, 0u, "the RV-3028 countdown needs no LPTIMER");
	zassert_equal(ev_count(EV_ARM_LPTIMER), 0u);
}

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
ZTEST(power_alif_se, test_bench_variants_default_to_the_documented_profile)
{
	sleep_plan_t  plan = { .mode          = ALP_POWER_MODE_STOP,
		                   .hw            = ALP_SOM_ARM_LPTIMER,
		                   .memory_blocks = ALP_AIPM_GEN2_BACKUP4K_MASK };
	off_profile_t out;

	zassert_ok(build_off_profile(&out, &g_live, &plan));
	zassert_equal(out.memory_blocks, ALP_AIPM_GEN2_BACKUP4K_MASK, "no MRAM / SERAM by default");
	zassert_equal(out.vtor_address, g_live.vtor_address, "the live vtor is preserved by default");
}
#endif

ZTEST(power_alif_se, test_the_record_says_whether_nsrst_can_be_trusted)
{
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_true((g_rec_at_set.armed_hw & ALP_SOM_REC_NSRST_TRUSTED) != 0u);

	/* A syndrome bit that does not clear: the next boot must not read it as a pin reset. */
	reset_fakes();
	g_nsrst_trusted     = false;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_equal(g_rec_at_set.armed_hw & ALP_SOM_REC_NSRST_TRUSTED, 0u);
	zassert_equal(g_rec_at_set.armed_hw & ALP_SOM_ARM_LPTIMER, ALP_SOM_ARM_LPTIMER);
}

/* ---- Elapsed-time gate on wake attribution (bench U8d) ------------------------------- */

static alp_som_pd_record_t lptimer_record(uint32_t armed_ms)
{
	return (alp_som_pd_record_t){ .armed_hw    = ALP_SOM_ARM_LPTIMER,
		                          .timed_bit   = ALP_POWER_WAKE_TIMER,
		                          .armed_ms    = armed_ms,
		                          .wake_source = ALP_POWER_WAKE_TIMER, /* from decode_early */
		                          .entry_rtc_s = 1000u,
		                          .entry_ccvr  = 5000u };
}

ZTEST(power_alif_se, test_a_genuine_lptimer_wake_is_kept)
{
	alp_som_pd_record_t rec = lptimer_record(500u);

	g_rtc_seconds = 1004u; /* 4 s: 0.5 s sleep + a ~3 s SES boot */
	g_ccvr_now    = 5000u + 8u;
	zassert_true(alp_som_power_wake_decode_i2c(&rec));
	zassert_equal(rec.wake_source, ALP_POWER_WAKE_TIMER);
}

ZTEST(power_alif_se, test_a_stale_lptimer_after_a_long_gap_is_not_a_wake)
{
	alp_som_pd_record_t rec = lptimer_record(500u);

	/* 76 s passed (the bench's falsely passing cycle): the periodic LPTIMER stays pending
	 * after any reset, so its latched status proves nothing. */
	g_rtc_seconds = 1076u;
	g_ccvr_now    = 5000u + 155u;
	zassert_true(alp_som_power_wake_decode_i2c(&rec));
	zassert_equal(rec.wake_source, 0u, "rejected");
	zassert_equal(rec.slept_ms, 76000u, "the elapsed time is still reported");
}

ZTEST(power_alif_se, test_each_witness_alone_can_reject)
{
	alp_som_pd_record_t rec = lptimer_record(500u);

	g_rtc_seconds_ok = false; /* no RV-3028: the LPRTC counter alone */
	g_ccvr_now       = 5000u + 155u;
	zassert_true(alp_som_power_wake_decode_i2c(&rec));
	zassert_equal(rec.wake_source, 0u);

	rec              = lptimer_record(500u);
	g_ccvr_now       = 5000u; /* no counter movement */
	g_rtc_seconds_ok = true;
	g_rtc_seconds    = 1090u; /* the RV-3028 alone */
	zassert_true(alp_som_power_wake_decode_i2c(&rec));
	zassert_equal(rec.wake_source, 0u);
}

ZTEST(power_alif_se, test_a_long_armed_interval_allows_a_long_gap)
{
	alp_som_pd_record_t rec = lptimer_record(5000u);

	g_rtc_seconds = 1009u; /* 5 s + 3 s boot + slack */
	g_ccvr_now    = 5000u + 20u;
	zassert_true(alp_som_power_wake_decode_i2c(&rec));
	zassert_equal(rec.wake_source, ALP_POWER_WAKE_TIMER);

	rec           = lptimer_record(5000u);
	g_rtc_seconds = 1040u;
	g_ccvr_now    = 5000u + 20u;
	zassert_true(alp_som_power_wake_decode_i2c(&rec));
	zassert_equal(rec.wake_source, 0u, "40 s is not a 5 s wake");
}

ZTEST(power_alif_se, test_the_rtc_countdown_is_not_gated_by_the_lptimer_rule)
{
	alp_som_pd_record_t rec = { .armed_hw    = ALP_SOM_ARM_RTC_TIMER,
		                        .timed_bit   = ALP_POWER_WAKE_TIMER,
		                        .armed_ms    = 3000u,
		                        .entry_rtc_s = 1000u };

	g_rtc_seconds = 1060u;
	g_rtc_flags   = RV3028C7_WAKE_TF;
	zassert_true(alp_som_power_wake_decode_i2c(&rec));
	zassert_equal(rec.wake_source, ALP_POWER_WAKE_TIMER, "the RV-3028 flag is its own proof");
}

ZTEST(power_alif_se, test_the_record_carries_the_entry_ccvr)
{
	g_ccvr_now          = 0xABCD0u;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_equal(g_rec_at_set.entry_ccvr, 0xABCD0u);
}

/* ---- Clock restore (bench U8c / U8d) ------------------------------------------------- */

ZTEST(power_alif_se, test_healthy_clocks_are_left_alone)
{
	zassert_equal(clock_restore(), 0);
	zassert_equal(g_run_set_calls, 0u, "no set_run_cfg when the PLL is locked and selected");
}

ZTEST(power_alif_se, test_an_all_rc_clock_tree_triggers_the_restore_with_a_complete_profile)
{
	/* After a reset inside the STOP: OSC_CTRL 0, PLL_LOCK_CTRL 0, PLL_CLK_SEL 0, ACLK 0x101. */
	g_cgu[ALIF_SE_CGU_OSC_CTRL]      = 0u;
	g_cgu[ALIF_SE_CGU_PLL_LOCK_CTRL] = 0u;
	g_cgu[ALIF_SE_CGU_PLL_CLK_SEL]   = 0u;
	g_cgu[ALIF_SE_CGU_ACLK_CTRL]     = 0x101u;
	zassert_false(clocks_healthy());

	zassert_equal(clock_restore(), 0);
	zassert_equal(g_run_set_calls, 1u);
	zassert_equal(g_run_set.power_domains, PD_SYST_MASK | PD_SSE700_AON_MASK, "the vendor set");
	zassert_equal(g_run_set.dcdc_voltage, 825u);
	zassert_equal(g_run_set.dcdc_mode, DCDC_MODE_PWM);
	zassert_equal(g_run_set.aon_clk_src, CLK_SRC_LFRC);
	zassert_equal(g_run_set.run_clk_src, CLK_SRC_PLL);
	zassert_equal(g_run_set.cpu_clk_freq, CLOCK_FREQUENCY_160MHZ);
	zassert_equal((int)g_run_set.scaled_clk_freq, 16);
	zassert_equal(g_run_set.memory_blocks,
	              ALP_AIPM_GEN2_MRAM_MASK | ALP_AIPM_GEN2_FWRAM_MASK | ALP_AIPM_GEN2_BACKUP4K_MASK,
	              "BACKUP4K (gen2 bit 21) is named, or the SE takes the Utility SRAM away");
	zassert_equal(g_run_set.ip_clock_gating, 0u);
	zassert_equal(g_run_set.phy_pwr_gating, 0u);
	zassert_equal(g_run_set.vdd_ioflex_3V3, IOFLEX_LEVEL_1V8);
	zassert_true(clocks_healthy(), "the PLL locked afterwards");
}

ZTEST(power_alif_se, test_each_unhealthy_condition_alone_triggers)
{
	g_cgu[ALIF_SE_CGU_PLL_LOCK_CTRL] = 0u; /* only the lock is gone */
	zassert_equal(clock_restore(), 0);
	zassert_equal(g_run_set_calls, 1u);

	reset_fakes();
	g_cgu[ALIF_SE_CGU_PLL_CLK_SEL] = 0x00110110u; /* only the select differs */
	zassert_equal(clock_restore(), 0);
	zassert_equal(g_run_set_calls, 1u);
}

ZTEST(power_alif_se, test_a_failed_restore_is_logged_not_fatal)
{
	g_cgu[ALIF_SE_CGU_PLL_LOCK_CTRL] = 0u;
	g_run_set_rc                     = -5;
	zassert_equal(clock_restore(), 0, "boot continues");
	zassert_equal(g_run_set_calls, 1u);
	zassert_false(clocks_healthy());

	/* The SE accepted it but the PLL never locked: also not fatal. */
	reset_fakes();
	g_cgu[ALIF_SE_CGU_PLL_LOCK_CTRL] = 0u;
	g_run_set_locks_pll              = false;
	zassert_equal(clock_restore(), 0);
}

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
ZTEST(power_alif_se, test_a_pll_that_never_locks_ends_the_wait_by_cycles_not_by_k_busy_wait)
{
	/* PRE_KERNEL_1: no SysTick, so k_busy_wait would spin forever on the target.  The host's
	 * works, so this test cannot lean on it: the fake cycle counter advances 1M cycles per
	 * read and the wait must give up after ~ALIF_SE_PLL_WAIT_CYCLES / 1M reads. */
	g_cgu[ALIF_SE_CGU_PLL_LOCK_CTRL] = 0u;
	g_run_set_locks_pll              = false;
	g_cycle_step                     = 1000000u;

	zassert_equal(clock_restore(), 0, "returns");
	zassert_true(g_cycle_reads >= ALIF_SE_PLL_WAIT_CYCLES / g_cycle_step,
	             "waited the full bound (%u reads)",
	             g_cycle_reads);
	zassert_true(g_cycle_reads <= ALIF_SE_PLL_WAIT_CYCLES / g_cycle_step + 4u,
	             "and stopped on the cycle bound (%u reads)",
	             g_cycle_reads);
	zassert_equal(g_boot_w[40], 2u, "reported as failed");
}
#endif

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
ZTEST(power_alif_se, test_the_in_progress_marker_is_written_before_the_se_call)
{
	g_cgu[ALIF_SE_CGU_PLL_LOCK_CTRL] = 0u;
	zassert_equal(clock_restore(), 0);
	zassert_equal(g_w40_at_se_call, 3u, "w40 = 3 when set_run_cfg ran");
	zassert_equal(g_boot_w[40], 1u, "and 1 once the PLL locked");
}
#endif

ZTEST(power_alif_se, test_the_run_profile_matches_the_cold_boot_profile)
{
	run_profile_t r;

	memset(&r, 0xA5, sizeof(r));
	build_run_profile(&r);
	const uint32_t *w = (const uint32_t *)&r;

	for (size_t i = 0; i < sizeof(r) / sizeof(uint32_t); ++i) {
		zassert_not_equal(w[i], POISON32, "run_profile_t word %u never assigned", (unsigned)i);
	}
}

/* ---- RV-3028 evidence at an /INT refusal ---------------------------------------------- */

ZTEST(power_alif_se, test_int_asserted_refusal_dumps_the_rtc_registers_once)
{
	g_int_level         = 1;
	g_rtc_int_armed     = true;
	g_rtc_regs[0]       = 0x08u; /* TF */
	g_rtc_regs[2]       = 0x10u; /* TIE */
	g_state.wake_bitmap = ALP_POWER_WAKE_RTC;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 0u, NULL), ALP_ERR_BUSY);
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 0u, NULL), ALP_ERR_BUSY);
	zassert_equal(g_stale_clears, 0u, "the caller's own alarm is never stopped");
	assert_no_side_effect();
}

/* ---- BKRAM-safe restore (bench U8e) ---------------------------------------------------- */

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
ZTEST(power_alif_se, test_the_restore_reasserts_the_bkram_retention_enables_then_unshadows)
{
	g_shadowed                       = true; /* shadow_begin ran at priority 0 */
	g_cgu[ALIF_SE_CGU_PLL_LOCK_CTRL] = 0u;
	g_regs[ALIF_SE_REG_ANA_REG1] &= ~ANA_REG1_RET_LDO_VBAT_EN; /* what the U8e SE call left */
	g_regs[ALIF_SE_REG_RET_CTRL] &= ~RET_CTRL_BKRAM;

	zassert_equal(clock_restore(), 0);
	zassert_not_equal(g_regs[ALIF_SE_REG_ANA_REG1] & ANA_REG1_RET_LDO_VBAT_EN, 0u);
	zassert_not_equal(g_regs[ALIF_SE_REG_RET_CTRL] & RET_CTRL_BKRAM, 0u);
	zassert_equal(g_shadow_end_calls, 1u);
	zassert_false(g_shadowed, "data written back only after the block was proved");
	zassert_equal(g_boot_w[52], 1u);
	zassert_equal(g_boot_w[40], 1u);
}
#endif

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
ZTEST(power_alif_se, test_a_dead_bkram_stays_shadowed_and_the_sleep_is_refused)
{
	g_shadowed                       = true;
	g_bkram_ok                       = false;
	g_cgu[ALIF_SE_CGU_PLL_LOCK_CTRL] = 0u;
	zassert_equal(clock_restore(), 0, "boot continues");
	zassert_true(g_shadowed, "the shadow stays authoritative");
	zassert_equal(g_boot_w[52], 2u);
	zassert_equal(g_boot_w[40], 2u, "reported as failed");

	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_ERR_NOT_READY);
	assert_no_side_effect();
}
#endif

ZTEST(power_alif_se, test_the_sleep_tests_bkram_before_touching_anything)
{
	g_bkram_ok          = false; /* live (not shadowed) but failing the write/readback */
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_ERR_NOT_READY);
	zassert_true(g_selftest_calls >= 1u);
	assert_no_side_effect();
}

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
ZTEST(power_alif_se, test_the_runtime_knobs_select_each_vendor_difference_in_the_off_profile)
{
	sleep_plan_t  plan = { .mode          = ALP_POWER_MODE_STOP,
		                   .hw            = ALP_SOM_ARM_LPTIMER,
		                   .memory_blocks = ALP_AIPM_GEN2_BACKUP4K_MASK };
	off_profile_t out;

	poison(&out);
	zassert_ok(build_off_profile(&out, &g_live, &plan));
	zassert_equal(out.vtor_address, g_live.vtor_address, "knobs off: the live vtor is kept");
	zassert_equal(out.memory_blocks, ALP_AIPM_GEN2_BACKUP4K_MASK);
	zassert_equal(out.stby_clk_freq, SCALED_FREQ_RC_STDBY_0_075_MHZ);

	alp_som_bench_knobs.vtor_self = true;
	poison(&out);
	zassert_ok(build_off_profile(&out, &g_live, &plan));
	zassert_equal(out.vtor_address, 0x80010400u, "SCB->VTOR");

	alp_som_bench_knobs.mram_seram = true;
	poison(&out);
	zassert_ok(build_off_profile(&out, &g_live, &plan));
	zassert_equal(out.memory_blocks,
	              ALP_AIPM_GEN2_BACKUP4K_MASK | ALP_AIPM_GEN2_MRAM_MASK | ALP_AIPM_GEN2_SERAM_MASK);

	alp_som_bench_knobs.stby_76_8 = true;
	poison(&out);
	zassert_ok(build_off_profile(&out, &g_live, &plan));
	zassert_equal(out.stby_clk_freq, SCALED_FREQ_RC_STDBY_76_8_MHZ);

	alp_som_bench_knobs.lfxo = true;
	g_state.wake_bitmap      = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_equal(g_set.aon_clk_src, CLK_SRC_LFXO, "the LFXO knob reaches the plan");
}
#endif

/* ---- U8g: the /INT check, the product OFF profile, the restore health check ------------ */

ZTEST(power_alif_se,
      test_a_wake_latched_by_an_earlier_unhandled_cycle_does_not_block_the_next_sleep)
{
	/* #2784: a countdown fired with nobody handling it (TF + TIE) and an alarm enable is still
	 * on (AF + AIE); /INT is low and the part keeps all of it across a power cycle.  The next
	 * countdown sleep clears it and proceeds, EVF (bit 1) is left alone. */
	g_int_level         = 1;
	g_rtc_regs[0]       = 0x08u | 0x04u | 0x02u; /* TF AF EVF */
	g_rtc_regs[2]       = 0x10u | 0x08u;         /* TIE AIE */
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_OK);
	zassert_equal(g_stale_clears, 1u);
	zassert_equal(g_rtc_regs[0] & 0x1Eu, 0x02u, "only EVF survives");
	zassert_equal(ev_count(EV_ARM_RTC_TIMER), 1u);
}

ZTEST(power_alif_se, test_a_wake_pending_from_the_current_arm_still_refuses)
{
	/* Stale state is cleaned BEFORE arming; a countdown that fires after the arm is not stale.
	 * The pending flags come from the fake registers, set by the arm itself. */
	g_int_level         = 1;
	g_rtc_regs[0]       = 0x08u;
	g_rtc_regs[2]       = 0x10u;
	g_fire_on_arm       = true;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_ERR_BUSY);
	zassert_equal(g_stale_clears, 1u);
	zassert_true(ev_pos(EV_CLEAR_STALE_WAKE) < ev_pos(EV_ARM_RTC_TIMER), "clear runs before arm");
	assert_unwound();
}

ZTEST(power_alif_se, test_a_requested_rtc_wake_keeps_the_callers_alarm_on_a_long_timed_wake)
{
	/* WAKE_RTC with a timed wake of >= 1 s: the countdown is this backend's, but the caller
	 * may have armed an alarm (AIE + AF) of its own, which must not be wiped. */
	g_rtc_regs[0]       = 0x04u | 0x08u; /* AF + stale TF */
	g_rtc_regs[2]       = 0x08u | 0x10u; /* AIE + stale TIE */
	g_state.wake_bitmap = ALP_POWER_WAKE_RTC | ALP_POWER_WAKE_TIMER;
	/* The caller's alarm has genuinely fired: the sleep refuses, and the alarm is intact. */
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_ERR_BUSY);
	zassert_true(g_last_keep_alarm);
	zassert_equal(g_rtc_regs[0] & 0x0Cu, 0x04u, "AF kept, stale TF cleared");
	zassert_equal(g_rtc_regs[2] & 0x18u, 0x08u, "AIE kept, stale TIE stopped");

	/* The caller's alarm is armed and has not fired: the sleep proceeds, AIE stays on. */
	reset_fakes();
	g_rtc_regs[0]       = 0x08u;
	g_rtc_regs[2]       = 0x08u | 0x10u;
	g_state.wake_bitmap = ALP_POWER_WAKE_RTC | ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_OK);
	zassert_equal(g_rtc_regs[2] & 0x18u, 0x08u, "AIE kept");

	/* Without WAKE_RTC the stale alarm goes too. */
	reset_fakes();
	g_rtc_regs[0]       = 0x04u;
	g_rtc_regs[2]       = 0x08u;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_OK);
	zassert_false(g_last_keep_alarm);
	zassert_equal(g_rtc_regs[0] & 0x04u, 0u);
	zassert_equal(g_rtc_regs[2] & 0x08u, 0u);
}

ZTEST(power_alif_se, test_a_lptimer_only_sleep_leaves_the_rtc_alone)
{
	g_rtc_regs[0]       = 0x08u;
	g_rtc_regs[2]       = 0x10u;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_equal(g_stale_clears, 0u);
}

ZTEST(power_alif_se, test_the_u8g_dump_is_not_a_refusal)
{
	/* The refusal dump of bench U8g: STATUS=0x10 (UF) CTRL1=0 CTRL2=0 EVENT_CTRL=0 EE35=0xc7
	 * EE37=0x90.  UF is set with UIE off, nothing is enabled: /INT is not asserted by the
	 * part even though the (gated, stale) pad read said so. */
	g_int_level   = 1;
	g_rtc_regs[0] = 0x10u;
	g_rtc_regs[4] = 0xc7u;
	g_rtc_regs[5] = 0x90u;

	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_OK);
	zassert_true(g_stale_clears >= 1u, "the stale UF is cleared while preparing");
}

ZTEST(power_alif_se, test_every_enabled_flag_still_counts_and_a_disabled_one_never_does)
{
	static const struct {
		uint8_t st, c2, e37;
		bool    refuse;
	} cases[] = {
		{ 0x02u, 0x04u, 0x00u, true },  /* EVF + EIE */
		{ 0x02u, 0x00u, 0x00u, false }, /* EVF, EIE off */
		{ 0x10u, 0x20u, 0x00u, true },  /* UF + UIE */
		{ 0x40u, 0x40u, 0x00u, true },  /* CLKF + CLKIE */
		{ 0x20u, 0x00u, 0x40u, true },  /* BSF + BSIE (EEPROM 37h) */
		{ 0x20u, 0x00u, 0x00u, false }, /* BSF, BSIE off */
		{ 0x04u, 0x08u, 0x00u, true },  /* AF + AIE */
		{ 0x08u, 0x00u, 0x00u, false }, /* TF, TIE off */
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); ++i) {
		reset_fakes();
		g_int_level         = 1;
		g_rtc_int_armed     = true; /* the caller's own alarm: nothing here is stale to us */
		g_rtc_regs[0]       = cases[i].st;
		g_rtc_regs[2]       = cases[i].c2;
		g_rtc_regs[5]       = cases[i].e37;
		g_state.wake_bitmap = ALP_POWER_WAKE_RTC;
		zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 0u, NULL),
		              cases[i].refuse ? ALP_ERR_BUSY : ALP_OK,
		              "case %u",
		              (unsigned)i);
	}
}

ZTEST(power_alif_se, test_a_low_pad_with_an_unreadable_rtc_still_refuses)
{
	g_int_level         = 1;
	g_rtc_int_armed     = true;
	g_rtc_regs_ok       = false;
	g_state.wake_bitmap = ALP_POWER_WAKE_RTC;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 0u, NULL), ALP_ERR_BUSY);
}

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
ZTEST(power_alif_se, test_the_product_default_is_the_vendor_off_profile)
{
	sleep_plan_t  plan = { .mode          = ALP_POWER_MODE_STOP,
		                   .hw            = ALP_SOM_ARM_LPTIMER,
		                   .memory_blocks = ALP_AIPM_GEN2_BACKUP4K_MASK };
	off_profile_t out;

	zassert_true(g_initial_knobs.vtor_self);
	zassert_true(g_initial_knobs.mram_seram);
	alp_som_bench_knobs = g_initial_knobs;
	poison(&out);
	zassert_ok(build_off_profile(&out, &g_live, &plan));
	zassert_equal(out.vtor_address, 0x80010400u, "SCB->VTOR, not the live value");
	zassert_equal(out.vtor_address_ns, 0x80010400u);
	zassert_equal(out.memory_blocks,
	              ALP_AIPM_GEN2_BACKUP4K_MASK | ALP_AIPM_GEN2_MRAM_MASK | ALP_AIPM_GEN2_SERAM_MASK);
}
#endif

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
ZTEST(power_alif_se, test_the_u8g_clock_tree_is_healthy)
{
	/* PLL locked and PLL_CLK_SEL 0x00100111 (ES0 [16] clear): the HE core's clock is right. */
	g_cgu[ALIF_SE_CGU_PLL_LOCK_CTRL] = 1u;
	g_cgu[ALIF_SE_CGU_PLL_CLK_SEL]   = 0x00100111u;
	zassert_true(clocks_healthy());
	zassert_equal(clock_restore(), 0);
	zassert_equal(g_run_set_calls, 0u, "no restore for a healthy tree");
	zassert_equal(g_boot_w[40], 0u);

	g_cgu[ALIF_SE_CGU_PLL_CLK_SEL] = 0x00000111u; /* ES1 [20] clear: not the PLL */
	zassert_false(clocks_healthy());
	g_cgu[ALIF_SE_CGU_PLL_CLK_SEL] = 0x00100110u; /* SYSREF [0] clear */
	zassert_false(clocks_healthy());
}
#endif

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
ZTEST(power_alif_se, test_w40_reflects_real_health_after_a_restore)
{
	g_cgu[ALIF_SE_CGU_PLL_LOCK_CTRL] = 0u;
	g_run_set_locks_pll              = false;
	zassert_equal(clock_restore(), 0);
	zassert_equal(g_boot_w[40], 2u, "the PLL never locked");

	reset_fakes();
	g_cgu[ALIF_SE_CGU_PLL_LOCK_CTRL] = 0u;
	zassert_equal(clock_restore(), 0);
	zassert_equal(g_boot_w[40], 1u, "locked and selected");
}
#endif

/* ---- A failed domain restore is not hidden (final gate review) -------------------------- */

ZTEST(power_alif_se, test_a_failed_restore_after_an_aborted_sleep_is_reported_and_kept)
{
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	g_restore_failed    = 0x4u; /* one domain would not come back */
	alp_power_wake_info_t info;

	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, &info), ALP_ERR_IO);
	zassert_equal(ev_count(EV_RESTORE), 1u);
}

ZTEST(power_alif_se, test_a_failed_restore_on_the_unwind_path_keeps_the_record)
{
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	g_set_rc            = -5;
	g_restore_failed    = 0x4u;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_ERR_IO);
	zassert_equal(ev_count(EV_ENTER), 0u);
	zassert_true(g_rec_valid, "the record is not cleared behind a failed restore");
}

ZTEST(power_alif_se, test_the_built_profile_is_checked_for_mram_and_seram)
{
	off_profile_t p;

	poison(&p);
	p.memory_blocks = ALP_AIPM_GEN2_BACKUP4K_MASK;
#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
	alp_som_bench_knobs.mram_seram = true; /* the product value */
#endif
	zassert_false(off_memory_ok(&p), "BKRAM alone is not enough");
	p.memory_blocks |= ALP_AIPM_GEN2_MRAM_MASK;
	zassert_false(off_memory_ok(&p), "MRAM without SERAM");
	p.memory_blocks |= ALP_AIPM_GEN2_SERAM_MASK;
	zassert_true(off_memory_ok(&p));

	/* And the profile build_off_profile() makes always passes. */
	sleep_plan_t plan = { .mode = ALP_POWER_MODE_STOP, .hw = ALP_SOM_ARM_LPTIMER };

	zassert_ok(build_off_profile(&p, &g_live, &plan));
	zassert_true(off_memory_ok(&p));
}

#ifndef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
/* The product configuration (no bench scratch option): the OFF fields are fixed, the diag
 * patches are no-ops, and nothing bench-only is reachable. */
ZTEST(power_alif_se, test_product_build_off_profile_is_the_vendor_one_and_not_switchable)
{
	sleep_plan_t  plan = { .mode          = ALP_POWER_MODE_STOP,
		                   .hw            = ALP_SOM_ARM_LPTIMER,
		                   .memory_blocks = ALP_AIPM_GEN2_BACKUP4K_MASK };
	off_profile_t out;

	poison(&out);
	zassert_ok(build_off_profile(&out, &g_live, &plan));
	zassert_true(OFF_VTOR_SELF);
	zassert_true(OFF_MRAM_SERAM);
	zassert_equal(out.vtor_address, 0x80010400u);
	zassert_equal(out.memory_blocks,
	              ALP_AIPM_GEN2_BACKUP4K_MASK | ALP_AIPM_GEN2_MRAM_MASK | ALP_AIPM_GEN2_SERAM_MASK);
	zassert_equal(out.stby_clk_freq, SCALED_FREQ_RC_STDBY_0_075_MHZ, "no 76.8 MHz knob");
	zassert_false(BENCH_KNOB(lfxo));
}

ZTEST(power_alif_se, test_product_build_runs_the_whole_sleep_and_the_restore)
{
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);

	g_cgu[ALIF_SE_CGU_PLL_LOCK_CTRL] = 0u;
	zassert_equal(clock_restore(), 0);
	zassert_equal(g_run_set_calls, 1u);
}
#endif
