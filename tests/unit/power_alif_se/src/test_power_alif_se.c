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

#define CONFIG_ALP_SDK_POWER_ALIF_SE 1

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

int se_service_get_off_cfg(off_profile_t *wp)
{
	ev(EV_GET_OFF_CFG);
	if (g_get_rc != 0) {
		return g_get_rc;
	}
	*wp = g_readback_overridden ? g_readback : g_stored;
	return 0;
}

int se_service_set_off_cfg(off_profile_t *wp)
{
	ev(EV_SET_OFF_CFG);
	g_rec_at_set = g_rec;
	if (g_set_rc != 0) {
		return g_set_rc;
	}
	g_set    = *wp;
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

alp_status_t alif_se_hw_wake_timer_arm(uint32_t ticks)
{
	if (g_arm_timer_rc != 0) {
		return (alp_status_t)g_arm_timer_rc;
	}
	ev(EV_ARM_LPTIMER);
	g_armed_ticks = ticks;
	return ALP_OK;
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

void alif_se_hw_enter_ewic(void)
{
	ev(EV_ENTER);
	g_enter_count++;
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

alp_status_t alp_som_power_restore(uint32_t *failed)
{
	ev(EV_RESTORE);
	if (failed != NULL) {
		*failed = 0;
	}
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

alp_status_t alp_som_power_rtc_seconds(uint32_t *seconds)
{
	*seconds = g_rtc_seconds;
	return g_rtc_seconds_ok ? ALP_OK : ALP_ERR_IO;
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

ZTEST_SUITE(power_alif_se, NULL, NULL, before, NULL, NULL);

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
	zassert_equal(out.power_domains, PD_SSE700_AON_MASK);
	zassert_equal(out.stby_clk_freq, SCALED_FREQ_RC_STDBY_76_8_MHZ);
}

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
	zassert_equal(g_armed_ticks, 16384u, "500 ms at 32768 Hz");
	zassert_equal(ev_count(EV_ARM_LPTIMER), 1u);
	zassert_equal(ev_count(EV_ARM_RTC_TIMER), 0u);
	zassert_equal(g_rec_at_set.armed_hw, ALP_SOM_ARM_LPTIMER);
	zassert_equal(g_set.wakeup_events, ALP_AIPM_GEN2_WE_LPTIMER0);
}

ZTEST(power_alif_se, test_timed_wake_boundary_999_and_1000)
{
	alp_power_wake_info_t info;

	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 999u, &info), ALP_OK);
	zassert_equal(g_armed_ticks, 32735u, "999 ms: LPTIMER");
	zassert_equal(ev_count(EV_ARM_RTC_TIMER), 0u);

	reset_fakes();
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 1000u, &info), ALP_OK);
	zassert_equal(ev_count(EV_ARM_LPTIMER), 0u, "1000 ms: RV-3028 countdown");
	zassert_equal(g_countdown_req, 1u);
	zassert_equal(g_rec_at_set.armed_hw, ALP_SOM_ARM_RTC_TIMER);
	zassert_equal(g_set.wakeup_events, ALP_AIPM_GEN2_WE_LPGPIO0);
	zassert_equal(g_rec_at_set.armed, ALP_POWER_WAKE_RTC, "the RV-3028 is the RTC wake");

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
	zassert_equal(g_rec_at_set.armed_hw, ALP_SOM_ARM_RTC_INT);
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

	/* RV-3028 /INT already low. */
	reset_fakes();
	g_int_level         = 1;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, NULL), ALP_ERR_BUSY);
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
	zassert_true(ev_pos(EV_QUIESCE) < ev_pos(EV_ARM_LPTIMER), "quiesce, then arm");
	zassert_true(ev_pos(EV_ARM_LPTIMER) < ev_pos(EV_SAVE_RECORD));
	zassert_true(ev_pos(EV_SAVE_RECORD) < ev_pos(EV_SET_OFF_CFG), "record before the SE call");
	zassert_true(ev_pos(EV_SET_OFF_CFG) < ev_pos(EV_ENTER));

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
	zassert_equal(g_set.power_domains, PD_SSE700_AON_MASK);
	zassert_equal(g_quiesce_mode, ALP_POWER_MODE_STANDBY);

	/* The record as the SE call saw it: this is what the cold-boot wake decodes. */
	zassert_equal(g_rec_at_set.mode, (uint32_t)ALP_POWER_MODE_STANDBY);
	zassert_equal(g_rec_at_set.armed, ALP_POWER_WAKE_RTC);
	zassert_equal(g_rec_at_set.armed_hw, ALP_SOM_ARM_RTC_TIMER);
	zassert_equal(g_rec_at_set.armed_ms, 2000u);
	zassert_equal(g_rec_at_set.entry_rtc_s, 777u);
	zassert_equal(g_rec_at_set.wake_source, 0u);
	zassert_equal(g_rec_at_set.slept_ms, 0u);

	/* An unreadable RV-3028 leaves the entry time unknown, not zero-by-accident. */
	reset_fakes();
	g_rtc_seconds_ok = false;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_equal(g_rec_at_set.entry_rtc_s, 0u);
	zassert_equal(g_rec_at_set.armed_hw, ALP_SOM_ARM_LPTIMER);
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
	g_arm_timer_rc = ALP_ERR_IO;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_ERR_IO);
	assert_unwound();
	zassert_equal(ev_count(EV_SET_OFF_CFG), 0u);

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
	zassert_true((g_regs[ALIF_SE_REG_RET_CTRL] & RET_CTRL_BKRAM) != 0u);
	zassert_true((g_regs[ALIF_SE_REG_ANA_REG1] & ANA_REG1_RET_LDO_VBAT_EN) != 0u);
	/* nothing the SE left set is cleared */
	zassert_equal(g_regs[ALIF_SE_REG_RET_CTRL] & 0x0002AAF0u, 0x0002AAF0u);
}

ZTEST(power_alif_se, test_untouched_registers_are_not_written)
{
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_equal(ev_count(EV_REG_WRITE), 0u, "already right: no write");
}

ZTEST(power_alif_se, test_tcm_retention_needs_ldo2_and_tcm_masks)
{
	g_state.retain = (alp_power_retain_t){ .level = ALP_POWER_RETAIN_TCM, .retain_kb = 64u };
	g_regs[ALIF_SE_REG_RET_CTRL] = RET_CTRL_BKRAM;
	g_regs[ALIF_SE_REG_ANA_REG1] = ANA_REG1_RET_LDO_VBAT_EN;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, NULL), ALP_OK);
	zassert_equal(g_set.memory_blocks, ALP_AIPM_GEN2_BACKUP4K_MASK | ALP_AIPM_GEN2_SRAM5_1_MASK);
	zassert_true((g_regs[ALIF_SE_REG_ANA_REG1] & ANA_REG1_RET_LDO_VDDMAIN_EN) != 0u);
	zassert_true((g_regs[ALIF_SE_REG_RET_CTRL] & (RET_CTRL_HETCM1 | RET_CTRL_HETCM2)) ==
	             (RET_CTRL_HETCM1 | RET_CTRL_HETCM2));
}

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
	zassert_equal((g_regs[ALIF_SE_REG_ANA_REG1] & ANA_REG1_CAP_CONT_MASK) >>
	                  ALP_ALIF_SE_SVD_ANA_REG1_XTAL32K_CAP_CONT_LSB,
	              63u);
	zassert_true((g_regs[ALIF_SE_REG_ANA_REG1] & ANA_REG1_XTAL32K_EN) != 0u);
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
	alp_som_pd_record_t rec = { .armed_hw = ALP_SOM_ARM_LPTIMER, .armed_ms = 500u };

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
	alp_som_pd_record_t rec = { .armed_hw = ALP_SOM_ARM_RTC_TIMER, .entry_rtc_s = 10u };

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
		                        .entry_rtc_s = 500u };

	g_rtc_seconds = 100u; /* the RV-3028 was set back mid-sleep */
	alp_som_power_wake_decode_i2c(&rec);
	zassert_equal(rec.slept_ms, 500u, "falls back to the nominal length");
}

ZTEST(power_alif_se, test_aborted_sleep_reports_the_source_that_fired)
{
	alp_power_wake_info_t info;

	g_timer_pending     = false;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	/* the fake enter returns; make the LPTIMER read latched only after arming */
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 500u, &info), ALP_OK);
	zassert_equal(info.wake_source, 0u);

	reset_fakes();
	g_rtc_flags         = RV3028C7_WAKE_TF;
	g_state.wake_bitmap = ALP_POWER_WAKE_TIMER;
	zassert_equal(se_request_sleep(&g_state, ALP_POWER_MODE_STOP, 3000u, &info), ALP_OK);
	zassert_equal(info.wake_source, ALP_POWER_WAKE_RTC);
	zassert_equal(info.realised_mode, ALP_POWER_MODE_RUN, "it never reached STOP");
}
