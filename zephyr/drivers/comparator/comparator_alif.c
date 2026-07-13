/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-2 (clean-room driver over the hal_alif analog lib) ======
 * Alif Ensemble analog comparator (HSCMP: cmp0..cmp3; the LPCMP variant is a
 * separate register block and is NOT handled here).  Implements the upstream
 * Zephyr v4.4 comparator class API (<zephyr/drivers/comparator.h>) for the
 * fork-native compatible "alif,cmp".
 *
 * PROVENANCE -- this is a clean-room driver, NOT a vendored fork copy.  Neither
 * hal_alif nor the zephyr_alif checkout available here ships a comparator class
 * driver `.c` (the fork carries only the binding, the DT nodes, and a sample).
 * So, unlike adc_alif.c / dac_alif.c (verbatim fork copies), the register
 * sequencing below was AUTHORED against the Alif DFP register definitions, each
 * value cited inline:
 *
 *   - register map (CMP_COMP_REG1 @0x00 .. CMP_INTERRUPT_MASK @0x24):
 *       alif-dfp Device/soc/AE822FA0E5597/include/rtss_he/soc.h `CMP_Type`
 *       (clean-room: only the offset VALUES are transcribed, not the struct).
 *   - field bit positions / enable bit / control-bit semantics:
 *       alif-dfp drivers/include/cmp.h + drivers/include/sys_ctrl_cmp.h.
 *   - input-select field values (COMP_HS_IN_M_SEL[3:2]=0x3 -> DAC6,
 *       COMP_HS_IN_P_SEL[1:0], COMP_HS_HYST[6:4], COMP_HS_EN bit 28,
 *       CMP_STATUS.CMP_VALUE bit 0): alif-dfp Debug/SVD CMP block.
 *   - clock-gate + VREF / DAC6 internal-reference helpers:
 *       hal_alif drivers/analog/include/analog_ctrl.h (pulled in by
 *       CONFIG_USE_ALIF_HAL_ANALOG, shared with adc_alif/dac_alif).  AE822 is
 *       aliasing-mode silicon (SOC_FEAT_HSCMP_REG_ALIASING=1), so the DAC6
 *       internal reference uses the SEPARATE DAC6 / ADC_VREF register blocks
 *       (the *_alias_mode helper), not CMP_COMP_REG2 -- and the CMP clock is
 *       ungated FIRST, before any CMP/DAC6/ADC_VREF register access (the fork's
 *       CMP_PowerControl order; both were init-fault root causes, see init).
 *
 * The v4.4 comparator class call sequence (set_trigger / set_trigger_callback /
 * get_output / trigger_is_pending) mirrors the sdk-alif reference sample
 * (samples/drivers/cmp/src/main.c).
 *
 * INIT-HANG ROOT CAUSE (fixed) -- unlike a one-shot peripheral (adc/dac), the
 * HSCMP analog compare is CONTINUOUS and LIVE the instant the core is enabled,
 * so its event line can already be asserted at init (more so with a floating /
 * unbiased input).  The init MUST NOT irq_enable() the NVIC line: an already-
 * asserted event self-retriggers the ISR (priority 0) into a storm that starves
 * the boot thread, so POST_KERNEL never completes and the boot banner never
 * prints (RAM console stays empty).  Init therefore only IRQ_CONNECT()s the ISR;
 * the NVIC line is enabled inside set_trigger() for a non-NONE trigger and
 * disabled on NONE -- the upstream comparator_stm32_comp.c order.
 *
 * vendor-ext, BENCH-UNVERIFIED.  Builds + links on the E8 he target; the
 * threshold/edge behaviour is a bench follow-up.  ADR 0017 Tier-2 INTERIM:
 * retire onto the fork comparator driver once it is repointed AND bench-verified.
 * See docs/adr/0017-alp-sdk-over-the-vendor-sdk.md.
 * ==============================================================================
 */

#define DT_DRV_COMPAT alif_cmp

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/comparator.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/device_mmio.h>
#include <soc_common.h>
#include "analog_ctrl.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(CMP, CONFIG_COMPARATOR_LOG_LEVEL);

/*
 * HSCMP register offsets, transcribed (offset VALUES only) from the Alif DFP
 * CMP_Type (alif-dfp Device/soc/AE822FA0E5597/include/rtss_he/soc.h:2488).
 */
#define CMP_COMP_REG1      0x00U /* CMP control register 1 (config + enable) */
#define CMP_COMP_REG2      0x04U /* CMP control register 2; on AE822 (aliasing) NOT the DAC6 ref */
#define CMP_POLARITY_CTRL  0x08U /* polarity control (invert result)         */
#define CMP_WINDOW_CTRL    0x0CU /* window (gating) control                  */
#define CMP_FILTER_CTRL    0x10U /* filter-tap control                       */
#define CMP_PRESCALER_CTRL 0x14U /* filter prescaler                         */
#define CMP_STATUS         0x18U /* status (CMP_VALUE output bit)            */
#define CMP_INTERRUPT_STATUS 0x20U /* interrupt status + clear                 */
#define CMP_INTERRUPT_MASK   0x24U /* interrupt mask                           */

/*
 * CMP_COMP_REG1 fields.  Bit positions from the DFP SVD CMP block
 * (alif-dfp Debug/SVD/AE822FA0E5597BS0_CM55_HE_View.svd, register CMP_COMP_REG1)
 * and alif-dfp drivers/include/sys_ctrl_cmp.h (COMP0_HS_* _Pos, CMP0_ENABLE).
 */
#define CMP_REG1_IN_P_SEL_POS 0U /* COMP_HS_IN_P_SEL [1:0] */
#define CMP_REG1_IN_P_SEL_MSK (0x3U << CMP_REG1_IN_P_SEL_POS)
#define CMP_REG1_IN_M_SEL_POS 2U /* COMP_HS_IN_M_SEL [3:2] */
#define CMP_REG1_IN_M_SEL_MSK (0x3U << CMP_REG1_IN_M_SEL_POS)
#define CMP_REG1_HYST_POS     4U /* COMP_HS_HYST [6:4], 6 mV/step */
#define CMP_REG1_HYST_MSK     (0x7U << CMP_REG1_HYST_POS)
#define CMP_REG1_HS_EN        BIT(28) /* COMP_HS_EN -- CMP0_ENABLE (1U<<28) */

/* COMP_HS_IN_M_SEL value selecting the on-chip DAC6 programmable reference as
 * the negative input -- the internal-reference path (no external pad).  SVD
 * CMP_COMP_REG1.COMP_HS_IN_M_SEL enumeratedValue 0x3 = "DAC6". */
#define CMP_IN_M_SEL_DAC6 0x3U

/* CMP_STATUS.CMP_VALUE -- the synchronized comparator output bit.  SVD
 * CMP_STATUS, bitRange [0:0]. */
#define CMP_STATUS_VALUE BIT(0)

/* Interrupt mask / clear.  alif-dfp drivers/include/cmp.h:29-30
 * (CMP_INT_MASK = 0x01, CMP_INTERRUPT_CLEAR = 0x01). */
#define CMP_INT_MASK_BIT  0x01U
#define CMP_INT_CLEAR_BIT 0x01U

/* Filter control: alif-dfp drivers/include/cmp.h:21,145 -- bit 0 enables the
 * filter, the tap count goes in bits [..8]. */
#define CMP_FILTER_CTRL_EN  BIT(0)
#define CMP_FILTER_TAPS_POS 8U

/*
 * driver_instance enum index -> CMP0_ENABLE clock-control bit in
 * CLKCTRL_PER_SLV->CMP_CTRL.  alif-dfp drivers/include/sys_ctrl_cmp.h:43-46:
 * CMP0=bit0, CMP1=bit4, CMP2=bit8, CMP3=bit12 (the HSCMP instances always also
 * need CMP0's bit 0 set -- the analog_ctrl helper sets bit 0 for us).  The
 * binding driver_instance enum order is LP,0,1,2,3.
 */
enum cmp_drv_instance {
	CMP_DRV_INSTANCE_LP = 0,
	CMP_DRV_INSTANCE_0,
	CMP_DRV_INSTANCE_1,
	CMP_DRV_INSTANCE_2,
	CMP_DRV_INSTANCE_3,
};

struct cmp_alif_config {
	DEVICE_MMIO_NAMED_ROM(cmp_reg);    /* per-instance HSCMP block */
	DEVICE_MMIO_NAMED_ROM(config_reg); /* CMP0 config block (REG2 / DAC6) */
	const struct pinctrl_dev_config *pcfg;
	void (*irq_config_func)(const struct device *dev);
	uint32_t irqn;        /* NVIC line, gated in set_trigger (NOT at init) */
	uint32_t drv_inst;    /* enum cmp_drv_instance */
	uint32_t in_p_sel;    /* 0..3  (positive_input enum idx) */
	uint32_t in_m_sel;    /* 0..3  (negative_input enum idx) */
	uint32_t hyst;        /* 0..7  (hysteresis_level enum idx) */
	uint32_t prescaler;   /* 0..0x3F */
	uint32_t filter_taps; /* 2..8, 0 = filter off */
	bool     polarity_en;
};

struct cmp_alif_data {
	DEVICE_MMIO_NAMED_RAM(cmp_reg);
	DEVICE_MMIO_NAMED_RAM(config_reg);
	comparator_callback_t   callback;
	void                   *user_data;
	enum comparator_trigger trigger;
};

/* DEVICE_MMIO_NAMED_* expand through DEV_CFG()/DEV_DATA(); the driver must
 * provide them (Zephyr device_mmio.h does not auto-define them). */
#define DEV_CFG(dev)  ((const struct cmp_alif_config *)((dev)->config))
#define DEV_DATA(dev) ((struct cmp_alif_data *)((dev)->data))

static inline uintptr_t cmp_base(const struct device *dev)
{
	return DEVICE_MMIO_NAMED_GET(dev, cmp_reg);
}

/* -------- upstream Zephyr v4.4 comparator_driver_api -------- */

static int cmp_alif_trigger_is_pending(const struct device *dev);

static int cmp_alif_get_output(const struct device *dev)
{
	/* CMP_STATUS.CMP_VALUE -- 1 = positive input above negative input. */
	return (sys_read32(cmp_base(dev) + CMP_STATUS) & CMP_STATUS_VALUE) ? 1 : 0;
}

static int cmp_alif_set_trigger(const struct device *dev, enum comparator_trigger trigger)
{
	const struct cmp_alif_config *config = dev->config;
	struct cmp_alif_data         *data   = dev->data;
	uintptr_t                     base   = cmp_base(dev);

	data->trigger = trigger;

	/* Gate the NVIC line FIRST -- the HSCMP analog compare is LIVE the moment
	 * the comparator is enabled, so its event line can already be asserted (an
	 * unbiased/floating input toggles continuously).  Disabling the NVIC line
	 * before re-arming prevents a self-retriggering ISR storm.  Mirrors the
	 * upstream comparator_stm32_comp.c set_trigger order (irq_disable -> arm ->
	 * irq_enable).  The NVIC line is left DISABLED out of init -- it is enabled
	 * here, and only for a non-NONE trigger. */
	irq_disable(config->irqn);

	/* Clear any latched event, then mask/unmask the single CMP interrupt at the
	 * IP level.  The HSCMP raises one line on a configured edge; edge-direction
	 * discrimination (RISING vs FALLING vs BOTH) is done in software in the ISR
	 * by re-reading CMP_STATUS.  NONE leaves both the IP mask and the NVIC line
	 * disabled. */
	sys_write32(CMP_INT_CLEAR_BIT, base + CMP_INTERRUPT_STATUS);

	if (trigger == COMPARATOR_TRIGGER_NONE) {
		sys_write32(CMP_INT_MASK_BIT, base + CMP_INTERRUPT_MASK);
	} else {
		/* cmp.h: writing 0 to the mask register ENABLES the interrupt. */
		sys_write32(0U, base + CMP_INTERRUPT_MASK);
		irq_enable(config->irqn);
	}

	return 0;
}

static int cmp_alif_set_trigger_callback(const struct device  *dev,
                                         comparator_callback_t callback,
                                         void                 *user_data)
{
	struct cmp_alif_data *data = dev->data;
	unsigned int          key  = irq_lock();

	data->callback  = callback;
	data->user_data = user_data;

	irq_unlock(key);

	/* Class contract: if a trigger is already pending, fire immediately. */
	if (callback != NULL && cmp_alif_trigger_is_pending(dev)) {
		callback(dev, user_data);
	}

	return 0;
}

static int cmp_alif_trigger_is_pending(const struct device *dev)
{
	uintptr_t base = cmp_base(dev);

	if (sys_read32(base + CMP_INTERRUPT_STATUS) & CMP_INT_CLEAR_BIT) {
		/* Pending: clear and report it (class contract). */
		sys_write32(CMP_INT_CLEAR_BIT, base + CMP_INTERRUPT_STATUS);
		return 1;
	}

	return 0;
}

static DEVICE_API(comparator, cmp_alif_driver_api) = {
	.get_output           = cmp_alif_get_output,
	.set_trigger          = cmp_alif_set_trigger,
	.set_trigger_callback = cmp_alif_set_trigger_callback,
	.trigger_is_pending   = cmp_alif_trigger_is_pending,
};

static void cmp_alif_isr(const struct device *dev)
{
	struct cmp_alif_data *data = dev->data;
	uintptr_t             base = cmp_base(dev);

	/* Acknowledge the latched event (cmp.h cmp_clear_interrupt()). */
	sys_write32(CMP_INT_CLEAR_BIT, base + CMP_INTERRUPT_STATUS);

	/* Software edge filter: the IP gives one event line; honour the
	 * configured trigger by re-reading the synchronized output. */
	if (data->callback != NULL && data->trigger != COMPARATOR_TRIGGER_NONE) {
		int out = cmp_alif_get_output(dev);

		switch (data->trigger) {
		case COMPARATOR_TRIGGER_RISING_EDGE:
			if (out == 1) {
				data->callback(dev, data->user_data);
			}
			break;
		case COMPARATOR_TRIGGER_FALLING_EDGE:
			if (out == 0) {
				data->callback(dev, data->user_data);
			}
			break;
		case COMPARATOR_TRIGGER_BOTH_EDGES:
		default:
			data->callback(dev, data->user_data);
			break;
		}
	}
}

static int cmp_alif_init(const struct device *dev)
{
	const struct cmp_alif_config *config = dev->config;
	struct cmp_alif_data         *data   = dev->data;
	uintptr_t                     base;
	uint32_t                      reg1;
	int                           err;

	/* Map both reg windows.  config_reg (the CMP0 config block) is mapped to
	 * keep the named-MMIO RAM/ROM slots consistent, but on AE822 it is NOT the
	 * DAC6 reference register (see step 2) -- the internal reference is the
	 * separate DAC6 / ADC_VREF blocks, so config_reg is unused at runtime. */
	DEVICE_MMIO_NAMED_MAP(dev, cmp_reg, K_MEM_CACHE_NONE);
	DEVICE_MMIO_NAMED_MAP(dev, config_reg, K_MEM_CACHE_NONE);

	base = cmp_base(dev);

	/* Mux the comparator input pads if the node carries a pinctrl group
	 * (the internal-reference smoke leaves it off). */
	if (config->pcfg != NULL) {
		err = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
		if (err != 0) {
			LOG_ERR("pinctrl apply failed (%d)", err);
			return err;
		}
	}

	/* 1. UNGATE THE CMP PERIPHERAL CLOCK FIRST.  The CMP / DAC6 / ADC_VREF
	 *    register blocks (all in the 0x4902_xxxx Expansion-Slave region) are
	 *    clock-gated out of reset; touching any of them before the clock is
	 *    ungated faults.  The fork ungates the clock as its very first action
	 *    (alif-dfp Alif_CMSIS/Source/Driver_CMP.c:170 CMP_PowerControl ->
	 *    enable_cmp_clk(), BEFORE AnalogConfig() and any CMP register write),
	 *    so we do the same here -- BEFORE the DAC6 / CMP register writes below.
	 *    CLKCTRL_PER_SLV->CMP_CTRL bit 0 = CMP0 clock (analog_ctrl.h:84
	 *    CMP_CTRL_CMP0_CLKEN (1U<<0); in AE822 aliasing mode each instance has
	 *    its own bit, bit0=CMP0 -- alif-dfp drivers/include/sys_ctrl_cmp.h:37).
	 *    Then enable the analog LDO + precision bandgap (VBAT_ANA_REG2).
	 *    Addresses from soc_common.h; same helper library adc_alif/dac_alif use. */
	unsigned int key = irq_lock();

	enable_analog_periph_clk((uintptr_t)CLKCTRL_PER_SLV_CMP_CTRL);
	enable_analog_peripherals((uintptr_t)ANA_VBAT_REG2);

	irq_unlock(key);

	/* 2. If the negative input is the on-chip DAC6 programmable reference,
	 *    turn DAC6 on so the comparator has an INTERNAL reference -- no
	 *    external pad needed.  On AE822 (SOC_FEAT_HSCMP_REG_ALIASING=1) DAC6
	 *    and the ADC VREF buffer are SEPARATE register blocks (0x4902A000 /
	 *    0x4902B000), NOT CMP_COMP_REG2 -- the fork's aliasing-mode path writes
	 *    ADC_VREF_REG |= ADC_VREF_BUF_EN then DAC6_REG |= DAC6_REF_VAL
	 *    (alif-dfp drivers/include/sys_ctrl_analog.h:84-88, the
	 *    SOC_FEAT_HSCMP_REG_ALIASING branch).  Use the matching hal_alif helper
	 *    enable_dac6_ref_voltage_alias_mode(adc_vref_base, dac6_reg)
	 *    (analog_ctrl.h:139); the previous enable_dac6_ref_voltage(cfg_base +
	 *    CMP_COMP_REG2) is the NON-aliasing path and wrote the wrong block on
	 *    AE822. */
	if (config->in_m_sel == CMP_IN_M_SEL_DAC6) {
		enable_dac6_ref_voltage_alias_mode((uintptr_t)ADC_VREF_REG, (uintptr_t)DAC6_REG);
	}

	/* 3. Polarity (invert result) -- CMP_POLARITY_CTRL. */
	sys_write32(config->polarity_en ? 1U : 0U, base + CMP_POLARITY_CTRL);

	/* 4. Optional digital filter -- bit 0 enables, taps in bits [..8]
	 *    (cmp.h cmp_set_filter_ctrl()). */
	if (config->filter_taps != 0U) {
		sys_write32(CMP_FILTER_CTRL_EN | (config->filter_taps << CMP_FILTER_TAPS_POS),
		            base + CMP_FILTER_CTRL);
		sys_write32(config->prescaler & 0x3FU, base + CMP_PRESCALER_CTRL);
	}

	/* 5. Program input-select + hysteresis, then enable the comparator, in a
	 *    single CMP_COMP_REG1 write (the enable bit lives in the same reg). */
	reg1 = ((config->in_p_sel << CMP_REG1_IN_P_SEL_POS) & CMP_REG1_IN_P_SEL_MSK) |
	       ((config->in_m_sel << CMP_REG1_IN_M_SEL_POS) & CMP_REG1_IN_M_SEL_MSK) |
	       ((config->hyst << CMP_REG1_HYST_POS) & CMP_REG1_HYST_MSK) | CMP_REG1_HS_EN;
	sys_write32(reg1, base + CMP_COMP_REG1);

	/* 6. Start with the IP interrupt MASKED and the latched event CLEARED.  The
	 *    comparator core is now live (HS_EN set above) so get_output() polling
	 *    works, but it must NOT deliver an interrupt yet: set_trigger() arms it.
	 *    Only CONNECT the ISR here; the NVIC line is enabled later in
	 *    set_trigger() -- enabling it now, while an unbiased input is toggling
	 *    the event line, would self-retrigger an ISR storm that hangs boot. */
	sys_write32(CMP_INT_MASK_BIT, base + CMP_INTERRUPT_MASK);
	sys_write32(CMP_INT_CLEAR_BIT, base + CMP_INTERRUPT_STATUS);

	data->callback  = NULL;
	data->user_data = NULL;
	data->trigger   = COMPARATOR_TRIGGER_NONE;

	config->irq_config_func(dev);

	LOG_DBG("alif cmp inst=%u in_p=%u in_m=%u hyst=%u",
	        config->drv_inst,
	        config->in_p_sel,
	        config->in_m_sel,
	        config->hyst);

	return 0;
}

/*
 * The binding enums are 0-based and ordered exactly as the register fields need:
 *   positive_input  : CMP_POS_IN0..IN3  -> 0..3  (COMP_HS_IN_P_SEL value)
 *   negative_input  : CMP_NEG_IN0..IN3  -> 0..3  (COMP_HS_IN_M_SEL value; 3=DAC6)
 *   hysteresis_level: "0mV".."42mV"     -> 0..7  (COMP_HS_HYST value, 6 mV/step)
 * so DT_INST_ENUM_IDX yields the register value directly.
 */
#define CMP_ALIF_INIT(n)                                                                           \
	IF_ENABLED(DT_INST_NODE_HAS_PROP(n, pinctrl_0), (PINCTRL_DT_INST_DEFINE(n)));                  \
                                                                                                   \
	static void cmp_alif_irq_config_##n(const struct device *dev)                                  \
	{                                                                                              \
		/* Connect the ISR but DO NOT irq_enable() here: the comparator is live      \
		 * once enabled, so enabling the NVIC line at init -- before any trigger is   \
		 * armed -- lets an already-asserted event self-retrigger into an ISR storm   \
		 * (priority 0) that starves the boot thread, so the banner never prints.     \
		 * The line is enabled in set_trigger() once a real trigger is armed.  */             \
		IRQ_CONNECT(                                                                               \
		    DT_INST_IRQN(n), DT_INST_IRQ(n, priority), cmp_alif_isr, DEVICE_DT_INST_GET(n), 0);    \
	}                                                                                              \
                                                                                                   \
	static struct cmp_alif_data cmp_alif_data_##n;                                                 \
                                                                                                   \
	static const struct cmp_alif_config cmp_alif_config_##n = {                                    \
		DEVICE_MMIO_NAMED_ROM_INIT_BY_NAME(cmp_reg, DT_DRV_INST(n)),                               \
		DEVICE_MMIO_NAMED_ROM_INIT_BY_NAME(config_reg, DT_DRV_INST(n)),                            \
		IF_ENABLED(DT_INST_NODE_HAS_PROP(n, pinctrl_0),                                            \
		           (.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n), ))                                  \
		    .irq_config_func = cmp_alif_irq_config_##n,                                            \
		.irqn                = DT_INST_IRQN(n),                                                    \
		.drv_inst            = DT_INST_ENUM_IDX(n, driver_instance),                               \
		.in_p_sel            = DT_INST_ENUM_IDX(n, positive_input),                                \
		.in_m_sel            = DT_INST_ENUM_IDX(n, negative_input),                                \
		.hyst                = DT_INST_ENUM_IDX(n, hysteresis_level),                              \
		.prescaler           = DT_INST_PROP_OR(n, prescaler, 0),                                   \
		.filter_taps         = DT_INST_PROP_OR(n, filter_taps, 0),                                 \
		.polarity_en         = DT_INST_PROP(n, polarity_en),                                       \
	};                                                                                             \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n,                                                                       \
	                      cmp_alif_init,                                                           \
	                      NULL,                                                                    \
	                      &cmp_alif_data_##n,                                                      \
	                      &cmp_alif_config_##n,                                                    \
	                      POST_KERNEL,                                                             \
	                      CONFIG_COMPARATOR_INIT_PRIORITY,                                         \
	                      &cmp_alif_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CMP_ALIF_INIT)
