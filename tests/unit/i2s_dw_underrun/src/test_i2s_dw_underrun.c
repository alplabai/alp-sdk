/* SPDX-License-Identifier: Apache-2.0 */
/*
 * alp-sdk issue #2149 (round 2 review finding 4): the real
 * zephyr/drivers/i2s/i2s_dw.c -- not a fake that only models the Zephyr I2S
 * trigger contract -- compiled directly into this test's translation unit
 * (see the #include below) and exercised against a RAM-backed register
 * block plus a counting fake clock_control device, in place of real
 * DesignWare I2S hardware. No DT node of compatible "snps,designware-i2s"
 * exists anywhere in this test, so DT_INST_FOREACH_STATUS_OKAY(I2S_DW_INIT)
 * at the bottom of i2s_dw.c expands to nothing: neither
 * i2s_dw_initialize() nor any DEVICE_DT_INST_DEFINE() instance is ever
 * wired up. Every static function the driver defines (tx_stream_start(),
 * i2s_tx_irq_handler(), i2s_suspend()/i2s_resume(), ...) is still compiled
 * and, being in the SAME translation unit as this file, directly callable
 * below -- this test drives configure()/write()/trigger() and the ISR
 * handler exactly the way finding 4 specifies, not through a fake.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/clock_control.h>

/* ---------------------------------------------------------------------
 * CMSIS register-access macros i2s_dw.h expects from a vendor
 * core_cmXX.h this test never pulls in -- native_sim's own soc.h (the
 * one i2s_dw.c includes unconditionally) defines none of these.
 * ------------------------------------------------------------------ */
#ifndef __IOM
#define __IOM volatile
#endif
#ifndef __OM
#define __OM volatile
#endif
#ifndef __IM
#define __IM volatile const
#endif
#ifndef __STATIC_INLINE
#define __STATIC_INLINE static inline
#endif
#ifndef _VAL2FLD
#define _VAL2FLD(field, value) (((value) << field##_Pos) & field##_Msk)
#endif
#ifndef _FLD2VAL
#define _FLD2VAL(field, value) (((value) & field##_Msk) >> field##_Pos)
#endif

/*
 * i2s_dw_initialize()'s k_sem_init() calls reference these Kconfig symbols
 * directly (not only through the DT-instantiation macro this test
 * deliberately never expands). CONFIG_I2S_DW itself depends on
 * DT_HAS_SNPS_DESIGNWARE_I2S_ENABLED, which this test intentionally has
 * none of, so Kconfig never defines them here -- i2s_dw_initialize()'s
 * body is still compiled (just never called) as part of this TU, so these
 * must exist at preprocess time regardless.
 */
#ifndef CONFIG_I2S_DW_RX_BLOCK_COUNT
#define CONFIG_I2S_DW_RX_BLOCK_COUNT 4
#endif
#ifndef CONFIG_I2S_DW_TX_BLOCK_COUNT
#define CONFIG_I2S_DW_TX_BLOCK_COUNT 4
#endif

/*
 * alp-sdk issue #2205: count every CER.CLKEN clear the driver makes. The
 * register block is plain RAM, so a 1->0->1 glitch inside one call is
 * invisible in the final register value; the #2149 hazard is exactly that
 * glitch (a codec latches SHUTDOWN on bit-clock loss). i2s_dw.h is
 * included first -- with the same headers i2s_dw.c includes ahead of it --
 * so its include guard skips it inside i2s_dw.c, and every
 * i2s_clock_disable() call in the driver body then goes through the
 * counting wrapper below. The driver source itself is unchanged.
 */
#include <soc.h>
#include <zephyr/drivers/pinctrl.h>
#include "../../../../zephyr/drivers/i2s/i2s_dw.h"

static int fake_cer_clears;

static inline void counted_clock_disable(const struct i2s_dw_cfg *i2s)
{
	fake_cer_clears++;
	i2s_clock_disable(i2s);
}

#define i2s_clock_disable(i2s) counted_clock_disable(i2s)

/*
 * alp-sdk issue #2205: count the driver's LOG_ERR() calls, so a case can
 * assert which log level a refused trigger chose. Same technique: the
 * logging header is included first, then LOG_ERR is replaced before
 * i2s_dw.c is compiled. The count reflects the macro the driver invoked,
 * whatever the logging subsystem is configured to print.
 */
#include <zephyr/logging/log.h>

static int fake_log_errs;

#undef LOG_ERR
#define LOG_ERR(...) (fake_log_errs++)

/* The real driver, compiled AS-IS. */
#include "../../../../zephyr/drivers/i2s/i2s_dw.c"

/* ---------------------------------------------------------------------
 * Fake clock_control device: counts on()/off()/set_rate()/configure()
 * calls instead of touching real hardware. clock_control_*() (see
 * <zephyr/drivers/clock_control.h>) call straight through dev->api,
 * tolerating a NULL op as -ENOSYS -- i2s_dw.c already tolerates that for
 * .configure()/.set_rate(), so only .on/.off/.set_rate/.configure need a
 * real stub here (i2s_suspend()/i2s_resume() call all four).
 * ------------------------------------------------------------------ */
static int fake_set_rate_calls;
static int fake_on_calls;
static int fake_off_calls;
static int fake_configure_calls;

static int fake_clk_set_rate(const struct device        *dev,
                             clock_control_subsys_t      sys,
                             clock_control_subsys_rate_t rate)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(sys);
	ARG_UNUSED(rate);
	fake_set_rate_calls++;
	return 0;
}

static int fake_clk_on(const struct device *dev, clock_control_subsys_t sys)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(sys);
	fake_on_calls++;
	return 0;
}

static int fake_clk_off(const struct device *dev, clock_control_subsys_t sys)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(sys);
	fake_off_calls++;
	return 0;
}

static int fake_clk_configure(const struct device *dev, clock_control_subsys_t sys, void *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(sys);
	ARG_UNUSED(data);
	fake_configure_calls++;
	return 0;
}

static const struct clock_control_driver_api fake_clk_api = {
	.on        = fake_clk_on,
	.off       = fake_clk_off,
	.set_rate  = fake_clk_set_rate,
	.configure = fake_clk_configure,
};

/*
 * alp-sdk issue #2179: test (r) below is the first case in this suite to
 * call i2s_dw_initialize(), which runs device_is_ready(i2s->clk_dev) --
 * and z_impl_device_is_ready() (zephyr/kernel/device.c) dereferences
 * dev->state unconditionally, so a device literal without one NULL-derefs
 * there. This fake carries a state reporting "init ran, returned 0".
 */
static struct device_state fake_clk_state = {
	.init_res    = 0,
	.initialized = true,
};

static const struct device fake_clk_dev = {
	.name  = "fake_i2s_clk",
	.api   = &fake_clk_api,
	.state = &fake_clk_state,
};

/* ---------------------------------------------------------------------
 * RAM-backed register block -- the "hardware" the real driver's __IOM
 * accessors (i2s_clock_enable(), i2s_tx_channel_disable(), ...) read and
 * write directly, in place of a real DesignWare I2S instance's MMIO.
 * ------------------------------------------------------------------ */
static struct I2S_Type fake_regs;

/*
 * alp-sdk issue #2179: ISR is declared __IM (volatile const) -- the driver
 * only ever reads it -- so a test presenting an asserted source has to cast
 * the qualifier away. Casting through uintptr_t rather than a bare
 * `(uint32_t *)` keeps this correct on native_sim/native/64, where a
 * pointer is 64-bit; this suite is the first to compile i2s_dw.c on a
 * 64-bit host, so every pointer cast it adds has to survive that.
 */
static void fake_assert_isr(uint32_t bits)
{
	*(volatile uint32_t *)(uintptr_t)&fake_regs.ISR = bits;
}

/*
 * alp-sdk issue #2205: the Alif E8 register model. The E8 SVD (peripheral
 * LPI2S) gives these reset values, and on the E8 TER bit 0 (TXCHENX) and
 * RER bit 0 (RXCHENX) are read-only 1 -- measured on E1M-AEN803 serial 2026W36-0002 i2s3:
 * RER and TER still read 0x00FFFF01 after the driver's init-time clears.
 * Bits 8-23 are the per-slot enables. The RAM cell records what the driver
 * last wrote; fake_ter()/fake_rer() return what the silicon returns. That
 * is enough to be honest: the driver never branches on a TER/RER read, and
 * its only accesses are `|= bit0` / `&= ~bit0` read-modify-writes, whose
 * result differs from the silicon's only in the read-only bit these
 * accessors force.
 */
#define E8_IER_RESET 0x00000F00U
#define E8_TER_RESET 0x00FFFF01U
#define E8_RER_RESET 0x00FFFF01U
#define E8_ISR_RESET 0x00000010U
#define E8_IMR_RESET 0x00000073U

static uint32_t fake_ter(void)
{
	return fake_regs.TER | I2S_TER_TXCHEN_Msk;
}

static uint32_t fake_rer(void)
{
	return fake_regs.RER | I2S_RER_RXCHEN_Msk;
}

static void fake_e8_reset(void)
{
	memset(&fake_regs, 0, sizeof(fake_regs));
	fake_regs.IER = E8_IER_RESET;
	fake_regs.TER = E8_TER_RESET;
	fake_regs.RER = E8_RER_RESET;
	fake_regs.IMR = E8_IMR_RESET;
	fake_assert_isr(E8_ISR_RESET);
}

#define TEST_BLOCK_BYTES 8U
#define TEST_SLAB_BLOCKS 4U

K_MEM_SLAB_DEFINE_STATIC(test_tx_slab, TEST_BLOCK_BYTES, TEST_SLAB_BLOCKS, 4);
K_MEM_SLAB_DEFINE_STATIC(test_rx_slab, TEST_BLOCK_BYTES, TEST_SLAB_BLOCKS, 4);

static struct queue_item tx_ring_storage[TEST_SLAB_BLOCKS + 1];
static struct queue_item rx_ring_storage[TEST_SLAB_BLOCKS + 1];

static const struct i2s_dw_cfg test_cfg = {
	.clk_dev             = &fake_clk_dev,
	.clkid               = (clock_control_subsys_t)0,
	.cfg.wss_len         = WSS_LEN,
	.cfg.tx_fifo_trg_lvl = TX_FIFO_TRG_LVL,
	.cfg.rx_fifo_trg_lvl = RX_FIFO_TRG_LVL,
	.paddr               = &fake_regs,
	.irq_config          = NULL,
};

static struct i2s_dw_data test_data;

static const struct device test_dev = {
	.name   = "i2s_dw_under_test",
	.config = &test_cfg,
	.data   = &test_data,
	.api    = &i2s_dw_driver_api,
};

/* ---------------------------------------------------------------------
 * alp-sdk issue #2179: a second device whose .irq_config CAPTURES the
 * register state at the exact moment i2s_dw_initialize() arms the NVIC.
 * On real hardware irq_config() is IRQ_CONNECT() + irq_enable(), i.e. the
 * first instant a source latched by the PREVIOUS image (this block is not
 * in the SYSRESETREQ reset domain) can be delivered -- so "was the block
 * quiesced and every interrupt masked BEFORE the NVIC was armed?" is only
 * answerable from inside this callback, never from the register file after
 * i2s_dw_initialize() has returned.
 * ------------------------------------------------------------------ */
static int      irq_config_calls;
static uint32_t imr_at_irq_config;
static uint32_t ier_at_irq_config;
static uint32_t irer_at_irq_config;
static uint32_t iter_at_irq_config;

static void capture_irq_config(const struct device *dev)
{
	ARG_UNUSED(dev);

	irq_config_calls++;
	imr_at_irq_config  = fake_regs.IMR;
	ier_at_irq_config  = fake_regs.IER;
	irer_at_irq_config = fake_regs.IRER;
	iter_at_irq_config = fake_regs.ITER;
}

static const struct i2s_dw_cfg test_init_cfg = {
	.clk_dev             = &fake_clk_dev,
	.clkid               = (clock_control_subsys_t)0,
	.cfg.wss_len         = WSS_LEN,
	.cfg.tx_fifo_trg_lvl = TX_FIFO_TRG_LVL,
	.cfg.rx_fifo_trg_lvl = RX_FIFO_TRG_LVL,
	.paddr               = &fake_regs,
	.irq_config          = capture_irq_config,
};

static const struct device test_init_dev = {
	.name   = "i2s_dw_init_under_test",
	.config = &test_init_cfg,
	.data   = &test_data,
	.api    = &i2s_dw_driver_api,
};

/* ---------------------------------------------------------------------
 * Per-test reset: mirrors i2s_dw_initialize()'s own wiring (the function
 * pointers, ring buffers and semaphores DEVICE_DT_INST_DEFINE()/
 * I2S_DW_INIT would normally set up) plus its boot sequence
 * (i2s_enable_controller() -- CER.CLKEN is 1 from here onward on real
 * hardware, which is exactly the precondition the restart-skip guard
 * under test cares about).
 * ------------------------------------------------------------------ */
/*
 * alp-sdk issue #2149 (round 2 review finding 4): i2s_pm_action() is
 * genuinely exercised in test (f), just never by name --
 * i2s_suspend()/i2s_resume() are called directly there instead of through
 * its switch -- so from this TU's point of view it is dead code and trips
 * twister's -Werror=unused-function without this reference.
 *
 * alp-sdk issue #2179: i2s_dw_initialize() and i2s_dw_isr() used to need
 * the same treatment (the #2149 cases drive one layer below them, calling
 * i2s_tx_irq_handler()/i2s_rx_irq_handler() directly and wiring the device
 * up by hand). Tests (l) through (r) now call both by name, so their
 * (void) references are gone.
 */
static void silence_unused_driver_entry_points(void)
{
	(void)i2s_pm_action;
}

static void dw_test_before(void *fixture)
{
	ARG_UNUSED(fixture);

	silence_unused_driver_entry_points();

	fake_e8_reset();
	fake_cer_clears      = 0;
	fake_log_errs        = 0;
	fake_set_rate_calls  = 0;
	fake_on_calls        = 0;
	fake_off_calls       = 0;
	fake_configure_calls = 0;

	/* alp-sdk issue #2179 */
	irq_config_calls   = 0;
	imr_at_irq_config  = 0;
	ier_at_irq_config  = 0;
	irer_at_irq_config = 0;
	iter_at_irq_config = 0;

	memset(&test_data, 0, sizeof(test_data));
	test_data.tx.stream_start        = tx_stream_start;
	test_data.tx.stream_disable      = tx_stream_disable;
	test_data.tx.queue_drop          = tx_queue_drop;
	test_data.tx.mem_block_queue.buf = tx_ring_storage;
	test_data.tx.mem_block_queue.len = ARRAY_SIZE(tx_ring_storage);
	test_data.rx.stream_start        = rx_stream_start;
	test_data.rx.stream_disable      = rx_stream_disable;
	test_data.rx.queue_drop          = rx_queue_drop;
	test_data.rx.mem_block_queue.buf = rx_ring_storage;
	test_data.rx.mem_block_queue.len = ARRAY_SIZE(rx_ring_storage);

	k_sem_init(&test_data.rx.sem, 0, CONFIG_I2S_DW_RX_BLOCK_COUNT);
	k_sem_init(&test_data.tx.sem, CONFIG_I2S_DW_TX_BLOCK_COUNT, CONFIG_I2S_DW_TX_BLOCK_COUNT);

	test_data.tx.state = I2S_STATE_NOT_READY;
	test_data.rx.state = I2S_STATE_NOT_READY;

	i2s_enable_controller(&test_dev);
}

/*
 * alp-sdk issue #2205: release whatever a test left behind, so a test that
 * aborts on an assertion before its own cleanup cannot starve the shared
 * slabs and fail every later test with it. DROP is valid from any state
 * but NOT_READY and frees both the in-flight block and the queue.
 */
static void dw_test_after(void *fixture)
{
	ARG_UNUSED(fixture);

	if (test_data.tx.state != I2S_STATE_NOT_READY) {
		(void)i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	}
	if (test_data.rx.state != I2S_STATE_NOT_READY) {
		(void)i2s_dw_trigger(&test_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	}
}

ZTEST_SUITE(i2s_dw_underrun, NULL, NULL, dw_test_before, dw_test_after, NULL);

/* ---------------------------------------------------------------------
 * Drive helpers -- thin wrappers over the real i2s_dw_configure()/
 * i2s_dw_write()/i2s_dw_trigger() entry points, asserting each step
 * itself succeeds so a helper failure never masquerades as the actual
 * assertion under test failing.
 * ------------------------------------------------------------------ */
static struct i2s_config make_cfg(uint32_t rate, struct k_mem_slab *slab)
{
	struct i2s_config cfg = {
		.word_size      = 16,
		.channels       = 2,
		.format         = 0,
		.options        = 0,
		.frame_clk_freq = rate,
		.mem_slab       = slab,
		.block_size     = TEST_BLOCK_BYTES,
		.timeout        = 100,
	};

	return cfg;
}

static void tx_configure(uint32_t rate)
{
	struct i2s_config cfg = make_cfg(rate, &test_tx_slab);
	int               rc  = i2s_dw_configure(&test_dev, I2S_DIR_TX, &cfg);

	zassert_equal(rc, 0, "tx configure failed: %d", rc);
}

static void tx_write_block(void)
{
	void *block;
	int   rc = k_mem_slab_alloc(&test_tx_slab, &block, K_NO_WAIT);

	zassert_equal(rc, 0, "tx slab alloc failed: %d", rc);
	memset(block, 0, TEST_BLOCK_BYTES);
	rc = i2s_dw_write(&test_dev, block, TEST_BLOCK_BYTES);
	zassert_equal(rc, 0, "tx write failed: %d", rc);
}

static void tx_start(void)
{
	int rc = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_START);

	zassert_equal(rc, 0, "tx start failed: %d", rc);
}

static void tx_prepare(void)
{
	int rc = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);

	zassert_equal(rc, 0, "tx prepare failed: %d", rc);
}

static void rx_configure(uint32_t rate)
{
	struct i2s_config cfg = make_cfg(rate, &test_rx_slab);
	int               rc  = i2s_dw_configure(&test_dev, I2S_DIR_RX, &cfg);

	zassert_equal(rc, 0, "rx configure failed: %d", rc);
}

static void rx_start(void)
{
	int rc = i2s_dw_trigger(&test_dev, I2S_DIR_RX, I2S_TRIGGER_START);

	zassert_equal(rc, 0, "rx start failed: %d", rc);
}

/* ---------------------------------------------------------------------
 * (a) After a queue-empty underrun: state ERROR, CER bit 0 == 1, ITER.TXEN
 *     == 0, TX interrupt disabled. (alp-sdk issue #2205: this used to
 *     assert TER bit 0 == 0, which the E8 cannot do -- TXCHENX is
 *     read-only there, so ITER.TXEN is what stops TX.)
 *
 * MUTATION-PROVEN against base 36bc7f03a (pre-#2149): on that base,
 * i2s_tx_irq_handler()'s underrun exit calls the plain tx_stream_disable()
 * (keep_clock hard-baked false), which clears CER.CLKEN -- the `CER bit
 * 0 == 1` assertion below fails on that base. See the PR report for the
 * mutation run (this fix's own i2s_dw.c restored to base, then reverted).
 * ------------------------------------------------------------------ */
ZTEST(i2s_dw_underrun, test_underrun_isr_exit_keeps_clock_disables_tx)
{
	tx_configure(16000);
	tx_write_block();
	tx_start();

	/* Drives the queue-empty underrun exactly the way the real IRQ
	 * does: consumes the one queued block, then finds the ring empty
	 * on the "next block" queue_get() and takes the underrun exit. */
	i2s_tx_irq_handler(&test_dev);

	zassert_equal(test_data.tx.state, I2S_STATE_ERROR, "state not ERROR after underrun");
	zassert_true((fake_regs.CER & I2S_CER_CLKEN_Msk) != 0,
	             "CER.CLKEN cleared on underrun -- clock not kept alive");
	zassert_true((fake_regs.ITER & I2S_ITER_TXEN_Msk) == 0, "ITER.TXEN still set after underrun");
	zassert_true((fake_regs.IMR & (I2S_IMR_TXFEM_Msk | I2S_IMR_TXFOM_Msk)) ==
	                 (I2S_IMR_TXFEM_Msk | I2S_IMR_TXFOM_Msk),
	             "TX interrupt not disabled after underrun");
}

/* (b) PREPARE, then a same-rate restart, does NOT call set_rate. */
ZTEST(i2s_dw_underrun, test_prepare_restart_same_rate_skips_set_rate)
{
	int before;

	tx_configure(16000);
	tx_write_block();
	tx_start();
	i2s_tx_irq_handler(&test_dev);
	tx_prepare();

	tx_write_block();
	before = fake_set_rate_calls;
	tx_start();

	zassert_equal(
	    fake_set_rate_calls, before, "set_rate called on a skip-eligible same-rate restart");
	zassert_true((fake_regs.CER & I2S_CER_CLKEN_Msk) != 0, "clock not kept on across restart");
}

/* (c) DROP clears CER bit 0. */
ZTEST(i2s_dw_underrun, test_drop_gates_clock)
{
	tx_configure(16000);
	tx_write_block();
	tx_start();
	zassert_true((fake_regs.CER & I2S_CER_CLKEN_Msk) != 0, "clock not on after start");

	int rc = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);

	zassert_equal(rc, 0, "drop failed: %d", rc);
	zassert_true((fake_regs.CER & I2S_CER_CLKEN_Msk) == 0, "CER.CLKEN not cleared by DROP");
}

/* (d) STOP/DRAIN paths gate the clock. */
ZTEST(i2s_dw_underrun, test_stop_gates_clock)
{
	tx_configure(16000);
	tx_write_block();
	tx_start();

	int rc = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_STOP);

	zassert_equal(rc, 0, "stop failed: %d", rc);
	zassert_true((fake_regs.CER & I2S_CER_CLKEN_Msk) == 0, "STOP left CER.CLKEN set");
}

ZTEST(i2s_dw_underrun, test_drain_gates_clock)
{
	tx_configure(16000);
	tx_write_block();
	tx_start();

	int rc = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);

	zassert_equal(rc, 0, "drain failed: %d", rc);
	zassert_true((fake_regs.CER & I2S_CER_CLKEN_Msk) == 0, "DRAIN left CER.CLKEN set");
}

/*
 * (e) alp-sdk issue #2150 (phase 1, round-3 review finding 1): this test
 * used to be named test_rx_start_invalidates_tx_restart_skip and asserted
 * that an RX start on the shared clock at a DIFFERENT rate, landing while
 * TX sat parked in READY with clk_restart_skip_ok still armed (the
 * PREPARE-retry window z_start() leaves TX in on a slow/stalled producer --
 * see src/backends/i2s/zephyr_drv.c), succeeded and forced a later TX
 * restart to reprogram. THAT ASSERTION ENCODED THE DEFECT: it exercised
 * tx_clock_is_live()'s old `state == I2S_STATE_ERROR && flag` qualifier,
 * which PREPARE's ERROR->READY move silently defeats (PREPARE does not
 * clear the flag), so the old, narrower check answered "not live" for
 * exactly this window and let RX silently retune the shared divider out
 * from under a TX still relying on it at the OLD rate -- reopening the
 * exact silent-amp hole #2149 closed. With finding 1's fix,
 * tx_clock_is_live() correctly reports TX live here (READY + flag armed +
 * CER.CLKEN still set), so the RX start must now be REFUSED with -EBUSY on
 * the mismatched rate -- the same outcome as test (k)'s already-RUNNING
 * case -- and clk_restart_skip_ok must survive untouched for the eventual
 * TX restart to still skip reprogramming.
 *
 * MUTATION-PROVEN: re-adding the old ERROR-only qualifier to
 * tx_clock_is_live() (`dev_data->tx.state == I2S_STATE_ERROR &&
 * dev_data->tx.clk_restart_skip_ok` in place of the state-independent
 * `dev_data->tx.clk_restart_skip_ok`) turns this RED -- the RX start wrongly
 * succeeds (rc == 0, not -EBUSY) and the subsequent TX restart wrongly
 * reprograms (fake_set_rate_calls advances) once rx_stream_start()
 * invalidates the flag on its way to the (wrongly) accepted reprogram.
 * Restored after the mutation run.
 */
ZTEST(i2s_dw_underrun, test_rx_start_rejected_preserves_tx_restart_skip)
{
	int               rx_rc;
	int               drop_rc;
	int               before;
	struct i2s_config rx_cfg;

	tx_configure(16000);
	tx_write_block();
	tx_start();
	i2s_tx_irq_handler(&test_dev);
	tx_prepare();

	rx_cfg = make_cfg(48000, &test_rx_slab);
	rx_rc  = i2s_dw_configure(&test_dev, I2S_DIR_RX, &rx_cfg);
	zassert_equal(rx_rc, 0, "rx configure failed: %d", rx_rc);
	rx_rc = i2s_dw_trigger(&test_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	zassert_equal(rx_rc,
	              -EBUSY,
	              "RX start on a different rate did not refuse while TX sat parked in READY "
	              "with clk_restart_skip_ok armed: %d",
	              rx_rc);

	tx_write_block();
	before = fake_set_rate_calls;
	tx_start();

	zassert_equal(fake_set_rate_calls,
	              before,
	              "TX restart reprogrammed after a correctly-refused RX start -- "
	              "clk_restart_skip_ok did not survive the rejection");

	/* Cleanup: free TX's outstanding block (never run to completion
	 * through the ISR, same reasoning as test (g)'s own cleanup) before
	 * the shared test_tx_slab runs dry for later tests in this suite. RX
	 * never allocated one -- the rejection above happens before
	 * rx_stream_start()'s k_mem_slab_alloc(), same as test (k). */
	drop_rc = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	zassert_equal(drop_rc, 0, "tx drop cleanup failed: %d", drop_rc);
}

/* (f) Resume, then start, reprograms. */
ZTEST(i2s_dw_underrun, test_resume_forces_reprogram_on_next_start)
{
	int rc;
	int before;

	tx_configure(16000);
	tx_write_block();
	tx_start();
	i2s_tx_irq_handler(&test_dev);
	tx_prepare();

	rc = i2s_suspend(&test_dev);
	zassert_equal(rc, 0, "suspend failed: %d", rc);
	zassert_true((fake_regs.CER & I2S_CER_CLKEN_Msk) == 0, "suspend left CER.CLKEN set");

	rc = i2s_resume(&test_dev);
	zassert_equal(rc, 0, "resume failed: %d", rc);
	zassert_true((fake_regs.CER & I2S_CER_CLKEN_Msk) != 0, "resume did not re-enable CER.CLKEN");

	tx_write_block();
	before = fake_set_rate_calls;
	tx_start();

	zassert_true(fake_set_rate_calls > before, "TX restart after resume skipped reprogramming");
}

/*
 * (g) A TX rate change in i2s_dw_configure() (round-2 review finding 1)
 * invalidates the one-shot restart-skip flag: TX underrun (arms the flag,
 * clock stays on) -> PREPARE -> i2s_dw_configure() at a DIFFERENT rate ->
 * write -> start must reprogram, not skip onto the OLD rate's divider.
 *
 * MUTATION-PROVEN: with i2s_dw_configure()'s
 * `stream->clk_restart_skip_ok = false;` line removed (the finding-1 fix
 * for this specific invalidation site), this goes RED -- the TX restart
 * wrongly finds the stale flag still armed and CER.CLKEN still set, so it
 * skips set_rate entirely and `fake_set_rate_calls > before` is false.
 * Restored after the mutation run; see the PR report.
 */
ZTEST(i2s_dw_underrun, test_configure_rate_change_invalidates_tx_restart_skip)
{
	int before;
	int rc;

	tx_configure(16000);
	tx_write_block();
	tx_start();
	i2s_tx_irq_handler(&test_dev);
	tx_prepare();

	tx_configure(48000);

	tx_write_block();
	before = fake_set_rate_calls;
	tx_start();

	/* Free the block DROP picked up by start() -- unlike (a), this test
	 * never runs the block to completion through the ISR, so without this
	 * the block leaks out of the shared test_tx_slab for the rest of the
	 * suite (finite K_MEM_SLAB_DEFINE_STATIC capacity). Run before the
	 * assertion so cleanup still happens if the assertion below aborts. */
	rc = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	zassert_equal(rc, 0, "drop failed: %d", rc);

	zassert_true(fake_set_rate_calls > before,
	             "TX restart skipped reprogramming after a rate change in configure()");
}

/*
 * (h) alp-sdk issue #2150 (phase 1): an RX teardown (STOP here, same
 * shared body as DRAIN/DROP/both RX-ISR error exits -- see
 * rx_stream_disable()) must NOT gate the shared bit clock while TX
 * depends on it. Since issue #2205 the RX START parks the RUNNING TX in
 * ERROR with clk_restart_skip_ok armed, so this passes through
 * tx_clock_is_live()'s flag half, not its RUNNING half. Reverting
 * rx_stream_disable()'s `if (!tx_live)` guard back to an unconditional
 * i2s_clock_disable() turns this RED -- CER.CLKEN reads 0 after the RX
 * stop although the parked TX still counts on it, which is the exact
 * silent-amp exposure #2150 phase 1 closes.
 */
ZTEST(i2s_dw_underrun, test_rx_stop_with_tx_running_keeps_clock)
{
	int  rc;
	bool clk_on_after_rx_stop;

	tx_configure(16000);
	tx_write_block();
	tx_start();

	rx_configure(16000);
	rx_start();

	rc = i2s_dw_trigger(&test_dev, I2S_DIR_RX, I2S_TRIGGER_STOP);
	zassert_equal(rc, 0, "rx stop failed: %d", rc);
	clk_on_after_rx_stop = (fake_regs.CER & I2S_CER_CLKEN_Msk) != 0;

	/* Cleanup: free TX's outstanding block before the shared slab runs
	 * dry for later tests in this suite. Run before the final assertion
	 * so cleanup still happens if that assertion aborts the test. */
	rc = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	zassert_equal(rc, 0, "tx drop cleanup failed: %d", rc);

	zassert_true(clk_on_after_rx_stop,
	             "RX stop gated the shared clock while the pre-empted TX still depended on it");
}

/*
 * (i) alp-sdk issue #2150 (phase 1): the same RX stop, but with NO TX
 * stream configured at all (tx.state == I2S_STATE_NOT_READY, so
 * tx_clock_is_live() is false) -- a plain RX-only user must see no change
 * from before this fix. Reverting rx_stream_disable()'s guard to always
 * skip (e.g. hard-coding tx_live true) turns this RED -- CER.CLKEN would
 * stay set after the RX stop with nothing else on the bus.
 */
ZTEST(i2s_dw_underrun, test_rx_stop_with_tx_idle_gates_clock)
{
	int rc;

	rx_configure(16000);
	rx_start();

	rc = i2s_dw_trigger(&test_dev, I2S_DIR_RX, I2S_TRIGGER_STOP);
	zassert_equal(rc, 0, "rx stop failed: %d", rc);

	zassert_true((fake_regs.CER & I2S_CER_CLKEN_Msk) == 0,
	             "RX stop left the clock enabled with no TX stream active");
}

/*
 * (j) alp-sdk issue #2150 (phase 1): RX starting while TX is RUNNING at
 * the SAME rate must not touch the shared divider -- TX already
 * configured and is relying on it. Reverting rx_stream_start()'s
 * `clk_needs_reprogram = !tx_live;` back to always-true turns this RED --
 * fake_set_rate_calls advances on the RX start even though TX is live at
 * the identical rate.
 *
 * MUTATION-PROVEN (round-3 review finding 4a): the fake_set_rate_calls
 * assertion alone covers only i2s_configure_clocksource() -- it does NOT
 * prove rx_stream_start()'s SECOND `if (clk_needs_reprogram)` guard (the
 * one around i2s_configure_clock(), which writes CCR directly with no
 * clock_control call at all) is also honoured. Deleting that second guard
 * left all 12 cases green before this fix; the CCR-sentinel assertion below
 * closes that hole -- with the guard deleted this now goes RED (CCR is
 * rewritten to the driver's own computed value, which happens to differ
 * from the sentinel). Restored after the mutation run.
 */
ZTEST(i2s_dw_underrun, test_rx_start_with_tx_running_same_rate_skips_reprogram)
{
	int            rc;
	int            before;
	int            set_rate_calls_after_rx_start;
	const uint32_t ccr_sentinel = 0xDEADBEEFU;

	tx_configure(16000);
	tx_write_block();
	tx_start();

	rx_configure(16000);
	/* Stamped AFTER tx_start() (which legitimately writes CCR on its own
	 * first, non-restart-skip start) and read back below -- proves
	 * rx_stream_start() itself leaves CCR untouched, not merely that it
	 * skips the clock_control_set_rate() call fake_set_rate_calls counts. */
	fake_regs.CCR = ccr_sentinel;
	before        = fake_set_rate_calls;
	rc            = i2s_dw_trigger(&test_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	zassert_equal(rc, 0, "rx start failed: %d", rc);
	set_rate_calls_after_rx_start = fake_set_rate_calls;

	/* Cleanup both streams before the shared slabs run dry for later
	 * tests in this suite. Run before the final assertion so cleanup
	 * still happens if that assertion aborts the test. */
	rc = i2s_dw_trigger(&test_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	zassert_equal(rc, 0, "rx drop cleanup failed: %d", rc);
	rc = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	zassert_equal(rc, 0, "tx drop cleanup failed: %d", rc);

	zassert_equal(set_rate_calls_after_rx_start,
	              before,
	              "RX start reprogrammed the shared divider while TX was RUNNING at the same rate");
	zassert_equal(fake_regs.CCR,
	              ccr_sentinel,
	              "RX start rewrote CCR directly while TX was RUNNING at the same rate");
}

/*
 * (k) alp-sdk issue #2150 (phase 1): RX starting while TX is RUNNING at a
 * DIFFERENT rate must refuse -- there is one CCR divider for both
 * directions, so RX cannot silently retune it out from under TX. Must
 * return -EBUSY rather than pretend to succeed, and must not touch the
 * divider at all in the process. Reverting rx_stream_start()'s
 * mismatched-rate guard (dropping the `return -EBUSY;` and always
 * reprogramming) turns this RED on both assertions -- rc comes back 0 and
 * fake_set_rate_calls advances.
 *
 * MUTATION-PROVEN (round-3 review finding 4b): neither prior assertion
 * proves the rejection happens BEFORE rx_stream_start()'s
 * k_mem_slab_alloc(), which both the code comment there and the changelog
 * assert. Moving the `return -EBUSY;` below the alloc call keeps rc and
 * fake_set_rate_calls exactly as this test already checks, while leaking
 * one RX block per rejection -- the real backend's 2-block slab
 * (src/backends/i2s/zephyr_drv.c) starves after two rejections. The
 * k_mem_slab_num_free_get() assertion below closes that hole: with the
 * early return moved past the alloc, this now goes RED (one fewer free
 * block after the rejected start). Restored after the mutation run.
 */
ZTEST(i2s_dw_underrun, test_rx_start_with_tx_running_different_rate_fails)
{
	int      rx_start_rc;
	int      drop_rc;
	int      before;
	int      set_rate_calls_after_rx_start;
	uint32_t rx_slab_free_before;
	uint32_t rx_slab_free_after;
	int32_t  tx_state_after_rx_start;
	uint32_t iter_after_rx_start;

	tx_configure(16000);
	tx_write_block();
	tx_start();

	rx_configure(48000);
	before                        = fake_set_rate_calls;
	rx_slab_free_before           = k_mem_slab_num_free_get(&test_rx_slab);
	rx_start_rc                   = i2s_dw_trigger(&test_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	set_rate_calls_after_rx_start = fake_set_rate_calls;
	rx_slab_free_after            = k_mem_slab_num_free_get(&test_rx_slab);
	tx_state_after_rx_start       = test_data.tx.state;
	iter_after_rx_start           = fake_regs.ITER;

	/* Cleanup: free TX's outstanding block. RX never allocated one --
	 * the rejection happens before rx_stream_start()'s
	 * k_mem_slab_alloc(). Run before the final assertions so cleanup
	 * still happens if either aborts the test. */
	drop_rc = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	zassert_equal(drop_rc, 0, "tx drop cleanup failed: %d", drop_rc);

	zassert_equal(rx_start_rc,
	              -EBUSY,
	              "RX start on a different rate than a live TX did not return -EBUSY: %d",
	              rx_start_rc);
	zassert_equal(set_rate_calls_after_rx_start,
	              before,
	              "RX start touched the shared divider despite being rejected");
	zassert_equal(rx_slab_free_after,
	              rx_slab_free_before,
	              "RX start allocated (and leaked) an rx slab block before returning -EBUSY");
	/* alp-sdk issue #2205: a REFUSED RX start must not pre-empt TX -- the
	 * park sits after the -EBUSY check. MUTATION-PROVEN: moving the park
	 * above that check turns these RED. */
	zassert_equal(tx_state_after_rx_start,
	              I2S_STATE_RUNNING,
	              "a rejected RX start pre-empted TX (state %d)",
	              tx_state_after_rx_start);
	zassert_equal(iter_after_rx_start & I2S_ITER_TXEN_Msk,
	              I2S_ITER_TXEN_Msk,
	              "a rejected RX start cleared ITER.TXEN");
}

/*
 * (l) alp-sdk issue #2179: TXFE asserted AND unmasked while
 * dev_data->dir == I2S_DIR_RX. Before this fix i2s_dw_isr() read ISR,
 * matched neither dir-gated branch, changed NOTHING, and returned -- and
 * because the source is level-held, the NVIC tail-chains straight back in,
 * which is a live core spinning in the ISR with no fault taken. The ISR
 * must now mask the source it cannot service, so the interrupt condition
 * is guaranteed to have changed by the time it returns.
 *
 * This is the reachable ordering from the bench: an image runs TX (leaving
 * TXFEM unmasked over a TX FIFO that is empty -- TXFE's resting state --
 * because i2s_enable_rx_interrupt() only clears RXDAM|RXFOM), then opens
 * RX, which repoints dir.
 */
ZTEST(i2s_dw_underrun, test_isr_txfe_with_dir_rx_masks_txfe)
{
	uint32_t imr_before;

	rx_configure(16000);
	zassert_equal(test_data.dir, I2S_DIR_RX, "rx configure did not set dir");

	/* "An earlier TX phase left TXFEM unmasked", then TXFE asserts. */
	fake_regs.IMR = 0U;
	fake_assert_isr(I2S_ISR_TXFE_Msk);
	imr_before = fake_regs.IMR;

	i2s_dw_isr(&test_dev);

	zassert_not_equal(fake_regs.IMR,
	                  imr_before,
	                  "the ISR returned without changing the interrupt condition -- the NVIC "
	                  "tail-chains straight back in");
	zassert_true((fake_regs.IMR & I2S_IMR_TXFEM_Msk) != 0,
	             "TXFE left asserted AND unmasked while dir == I2S_DIR_RX");
}

/*
 * (m) alp-sdk issue #2179: the mirror of (l). RXDA asserted and unmasked
 * while dir == I2S_DIR_TX is the quieter half -- RXDA needs data to
 * arrive, and tx_stream_start() disables the RX channel -- but the guard
 * is about the shape (a source no branch will service), not the bit.
 */
ZTEST(i2s_dw_underrun, test_isr_rxda_with_dir_tx_masks_rxda)
{
	uint32_t imr_before;

	tx_configure(16000);
	zassert_equal(test_data.dir, I2S_DIR_TX, "tx configure did not set dir");

	fake_regs.IMR = 0U;
	fake_assert_isr(I2S_ISR_RXDA_Msk);
	imr_before = fake_regs.IMR;

	i2s_dw_isr(&test_dev);

	zassert_not_equal(
	    fake_regs.IMR, imr_before, "the ISR returned without changing the interrupt condition");
	zassert_true((fake_regs.IMR & I2S_IMR_RXDAM_Msk) != 0,
	             "RXDA left asserted AND unmasked while dir == I2S_DIR_TX");
}

/*
 * (n) alp-sdk issue #2179, the no-regression half: a source the dir gate
 * DOES route to its handler must still be serviced normally and must NOT
 * be masked. A guard that simply masked everything asserted would pass (l)
 * and (m) while killing the working RX stream on its first interrupt --
 * this is the case that rejects it.
 */
ZTEST(i2s_dw_underrun, test_isr_rxda_with_dir_rx_is_serviced_not_masked)
{
	uint32_t imr_after_isr;
	uint32_t rx_offset_after_isr;
	int      rc;

	rx_configure(16000);
	rx_start();
	zassert_true((fake_regs.IMR & I2S_IMR_RXDAM_Msk) == 0,
	             "rx start left its own RX interrupt masked");

	fake_assert_isr(I2S_ISR_RXDA_Msk);
	i2s_dw_isr(&test_dev);
	imr_after_isr       = fake_regs.IMR;
	rx_offset_after_isr = test_data.rx.mem_block_offset;

	/* Cleanup before the assertions so it still runs if one aborts: the
	 * handler queued the filled block and allocated a fresh one. */
	rc = i2s_dw_trigger(&test_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	zassert_equal(rc, 0, "rx drop cleanup failed: %d", rc);

	zassert_true((imr_after_isr & I2S_IMR_RXDAM_Msk) == 0,
	             "the ISR masked the RX source it had just serviced");
	zassert_equal(rx_offset_after_isr,
	              0U,
	              "the RX handler did not run to a completed block (offset %u)",
	              rx_offset_after_isr);
}

/*
 * (o) alp-sdk issue #2179: rx_stream_start() masks TX's interrupts
 * alongside the TX channel disable it already did. Disabling the TX
 * channel does not deassert TXFE (an empty FIFO is what asserts it), and
 * i2s_enable_rx_interrupt() clears only RXDAM|RXFOM, so TXFEM used to
 * survive an earlier TX phase into this RX stream -- the precondition
 * test (l) then spins on.
 */
ZTEST(i2s_dw_underrun, test_rx_start_masks_tx_interrupt)
{
	uint32_t imr_after_rx_start;
	int      rc;

	/* "An earlier TX phase left TXFEM unmasked." */
	fake_regs.IMR = 0U;

	rx_configure(16000);
	rx_start();
	imr_after_rx_start = fake_regs.IMR;

	rc = i2s_dw_trigger(&test_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	zassert_equal(rc, 0, "rx drop cleanup failed: %d", rc);

	zassert_equal(imr_after_rx_start & (I2S_IMR_TXFEM_Msk | I2S_IMR_TXFOM_Msk),
	              I2S_IMR_TXFEM_Msk | I2S_IMR_TXFOM_Msk,
	              "rx_stream_start() left TX's interrupts unmasked (IMR=0x%08x)",
	              imr_after_rx_start);
	zassert_equal(imr_after_rx_start & I2S_IMR_RXDAM_Msk,
	              0U,
	              "rx_stream_start() masked its own RX interrupt");
}

/*
 * (p) alp-sdk issue #2179: the mirror of (o) -- tx_stream_start() masks
 * RX's interrupts alongside the RX channel disable it already did.
 */
ZTEST(i2s_dw_underrun, test_tx_start_masks_rx_interrupt)
{
	uint32_t imr_after_tx_start;
	int      rc;

	fake_regs.IMR = 0U;

	tx_configure(16000);
	tx_write_block();
	tx_start();
	imr_after_tx_start = fake_regs.IMR;

	rc = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	zassert_equal(rc, 0, "tx drop cleanup failed: %d", rc);

	zassert_equal(imr_after_tx_start & (I2S_IMR_RXDAM_Msk | I2S_IMR_RXFOM_Msk),
	              I2S_IMR_RXDAM_Msk | I2S_IMR_RXFOM_Msk,
	              "tx_stream_start() left RX's interrupts unmasked (IMR=0x%08x)",
	              imr_after_tx_start);
	zassert_equal(imr_after_tx_start & I2S_IMR_TXFEM_Msk,
	              0U,
	              "tx_stream_start() masked its own TX interrupt");
}

/*
 * (q) alp-sdk issue #2179: dev_data->dir used to be assigned as the FIRST
 * statement of i2s_dw_configure(), before the switch validated anything --
 * so a call that went on to fail still repointed the one field
 * i2s_dw_isr() consults, permanently, while changing nothing else.
 * I2S_DIR_BOTH is the worst of the three failures: it leaves dir matching
 * NEITHER ISR branch, so from then on every I2S interrupt is unserviced.
 */
ZTEST(i2s_dw_underrun, test_failed_configure_leaves_dir_unchanged)
{
	struct i2s_config cfg = make_cfg(16000, &test_rx_slab);
	int               rc;

	tx_configure(16000);
	zassert_equal(test_data.dir, I2S_DIR_TX, "tx configure did not set dir");

	rc = i2s_dw_configure(&test_dev, I2S_DIR_BOTH, &cfg);
	zassert_equal(rc, -ENOSYS, "I2S_DIR_BOTH did not return -ENOSYS: %d", rc);
	zassert_equal(test_data.dir,
	              I2S_DIR_TX,
	              "a failed I2S_DIR_BOTH configure() repointed the ISR direction gate at a "
	              "direction neither ISR branch matches");

	rc = i2s_dw_configure(&test_dev, (enum i2s_dir)(I2S_DIR_BOTH + 1), &cfg);
	zassert_equal(rc, -EINVAL, "an invalid direction did not return -EINVAL: %d", rc);
	zassert_equal(test_data.dir,
	              I2S_DIR_TX,
	              "a failed invalid-direction configure() repointed the ISR direction gate");
}

/*
 * (r) alp-sdk issue #2179: i2s_dw_initialize() must quiesce the block and
 * mask every interrupt BEFORE irq_config() arms the NVIC. The I2S block is
 * NOT in the SYSRESETREQ reset domain -- measured on E1M-AEN803 serial 2026W36-0002, CER
 * and TER both survived a J-Link `loadbin`'s implicit SYSRESETREQ at
 * 0x00000001 (the all-zero read after a RESETPIN reset was an unclocked
 * block, not a reset value -- issue #2205) -- so a
 * fresh image inherits the previous one's IER/IMR/IRER/ITER/RER/TER. The
 * old order armed the NVIC first and masked two statements later, with
 * dev_data->dir fresh from BSS as 0 (== I2S_DIR_RX) and no stream
 * configured at all.
 *
 * The assertions read the state captured INSIDE irq_config(), not the
 * register file after init returns -- that is the only instant the
 * ordering is observable.
 *
 * alp-sdk issue #2205: this used to assert RER.RXCHEN == 0 and
 * TER.TXCHEN == 0 as well. On the E8 both are read-only 1 (see
 * fake_ter()/fake_rer()), so those asserts are gone -- IER/IRER/ITER are
 * what quiesce the block.
 */
ZTEST(i2s_dw_underrun, test_initialize_quiesces_before_arming_irq)
{
	const uint32_t all_masked =
	    I2S_IMR_RXDAM_Msk | I2S_IMR_RXFOM_Msk | I2S_IMR_TXFEM_Msk | I2S_IMR_TXFOM_Msk;
	int rc;

	/* Model the worst case a warm reset can hand over: block and both
	 * channels enabled, every interrupt unmasked. */
	fake_regs.IER  = I2S_IER_IEN_Msk;
	fake_regs.IRER = I2S_IRER_RXEN_Msk;
	fake_regs.ITER = I2S_ITER_TXEN_Msk;
	fake_regs.RER  = I2S_RER_RXCHEN_Msk;
	fake_regs.TER  = I2S_TER_TXCHEN_Msk;
	fake_regs.IMR  = 0U;

	rc = i2s_dw_initialize(&test_init_dev);
	zassert_equal(rc, 0, "i2s_dw_initialize failed: %d", rc);
	zassert_equal(irq_config_calls, 1, "irq_config not called exactly once: %d", irq_config_calls);

	zassert_equal(imr_at_irq_config & all_masked,
	              all_masked,
	              "NVIC armed with an I2S source still unmasked (IMR=0x%08x)",
	              imr_at_irq_config);
	zassert_equal(ier_at_irq_config & I2S_IER_IEN_Msk, 0U, "NVIC armed with IER.IEN still set");
	zassert_equal(
	    irer_at_irq_config & I2S_IRER_RXEN_Msk, 0U, "NVIC armed with IRER.RXEN still set");
	zassert_equal(
	    iter_at_irq_config & I2S_ITER_TXEN_Msk, 0U, "NVIC armed with ITER.TXEN still set");

	/* The #2149/#2150 invariant is unchanged: CER.CLKEN reads 1 from
	 * i2s_dw_initialize() onward -- it is just set a few statements later
	 * now, by the i2s_enable_controller() call this reordering moved
	 * below irq_config(). */
	zassert_true((fake_regs.CER & I2S_CER_CLKEN_Msk) != 0,
	             "i2s_dw_initialize() left CER.CLKEN clear");
}

/*
 * (s) alp-sdk issue #2205: i2s_dw_initialize() writes the FULL named IMR
 * mask, including the E8's TXFUM (bit 6). It used to set only bits
 * 0/1/4/5 by read-modify-write, so a previous image's unmask of bit 6 --
 * the block keeps its registers across a warm reset -- would have survived
 * into every boot, and nothing in this driver services TXFU. (Nothing
 * in-tree unmasks bit 6; this is the hole, not an observed fault.) Bits 2-3 have no field
 * in either SVD and must stay 0.
 *
 * MUTATION-PROVEN: restoring the two i2s_disable_tx_interrupt()/
 * i2s_disable_rx_interrupt() calls in place of the full write turns this
 * RED (IMR 0x00000033 at irq_config()).
 */
ZTEST(i2s_dw_underrun, test_initialize_masks_inherited_txfu_unmask)
{
	int rc;

	/* The previous image left every source unmasked, TXFUM included. */
	fake_regs.IMR = 0U;

	rc = i2s_dw_initialize(&test_init_dev);
	zassert_equal(rc, 0, "i2s_dw_initialize failed: %d", rc);
	zassert_equal(imr_at_irq_config,
	              I2S_IMR_ALL_Msk,
	              "NVIC armed with IMR=0x%08x, not the full named mask 0x%08x",
	              imr_at_irq_config,
	              (uint32_t)I2S_IMR_ALL_Msk);
}

/*
 * (t) alp-sdk issue #2205: i2s_dw_isr() masks EVERY unserviced source, not
 * only TXFE/RXDA. TXFU (bit 6) has no service branch, so an unmasked,
 * latched TXFU would otherwise leave the ISR without changing anything --
 * the #2179 storm shape. Only the unserviced bit is masked.
 *
 * MUTATION-PROVEN: restoring the TXFE-only/RXDA-only guard turns this RED
 * (IMR stays 0).
 */
ZTEST(i2s_dw_underrun, test_isr_masks_unserviced_txfu)
{
	tx_configure(16000);

	fake_regs.IMR = 0U;
	fake_assert_isr(I2S_ISR_TXFU_Msk);

	i2s_dw_isr(&test_dev);

	zassert_equal(fake_regs.IMR,
	              I2S_IMR_TXFUM_Msk,
	              "unserviced TXFU not masked, or more than it masked (IMR=0x%08x)",
	              fake_regs.IMR);
}

/*
 * (u) alp-sdk issue #2205: dev_data->dir follows START. The alp backend
 * opens TX (configure(TX)), then RX (configure(RX), which repoints dir),
 * runs and stops an RX session, then restarts TX without configuring it
 * again. dir used to stay I2S_DIR_RX, so the ISR never serviced the
 * restarted TX: its TXFE was masked as unserviced and the stream stalled.
 *
 * MUTATION-PROVEN: deleting `dev_data->dir = I2S_DIR_TX;` from
 * tx_stream_start() turns this RED (dir stays RX, TXFEM masked).
 */
ZTEST(i2s_dw_underrun, test_tx_restart_after_rx_session_is_serviced)
{
	enum i2s_dir dir_after_tx_start;
	uint32_t     imr_after_isr;
	int32_t      tx_state_after_isr;
	int          rc;

	tx_configure(16000);
	rx_configure(16000);
	rx_start();
	rc = i2s_dw_trigger(&test_dev, I2S_DIR_RX, I2S_TRIGGER_STOP);
	zassert_equal(rc, 0, "rx stop failed: %d", rc);

	tx_write_block();
	tx_write_block();
	tx_start();
	dir_after_tx_start = test_data.dir;

	fake_assert_isr(I2S_ISR_TXFE_Msk);
	i2s_dw_isr(&test_dev);
	imr_after_isr      = fake_regs.IMR;
	tx_state_after_isr = test_data.tx.state;

	rc = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	zassert_equal(rc, 0, "tx drop cleanup failed: %d", rc);

	zassert_equal(dir_after_tx_start, I2S_DIR_TX, "TX START did not point dir at TX");
	zassert_equal(imr_after_isr & I2S_IMR_TXFEM_Msk,
	              0U,
	              "the restarted TX's TXFE was masked as unserviced (IMR=0x%08x)",
	              imr_after_isr);
	zassert_equal(tx_state_after_isr,
	              I2S_STATE_RUNNING,
	              "the TX handler did not run to the second block (state %d)",
	              tx_state_after_isr);
}

/*
 * (v) alp-sdk issue #2205: the mirror of (u) -- RX restarted after a TX
 * session is serviced too.
 *
 * MUTATION-PROVEN: deleting `dev_data->dir = I2S_DIR_RX;` from
 * rx_stream_start() turns this RED.
 */
ZTEST(i2s_dw_underrun, test_rx_restart_after_tx_session_is_serviced)
{
	enum i2s_dir dir_after_rx_start;
	uint32_t     imr_after_isr;
	int          rc;

	rx_configure(16000);
	tx_configure(16000);
	tx_write_block();
	tx_start();
	rc = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	zassert_equal(rc, 0, "tx drop failed: %d", rc);

	rx_start();
	dir_after_rx_start = test_data.dir;

	fake_assert_isr(I2S_ISR_RXDA_Msk);
	i2s_dw_isr(&test_dev);
	imr_after_isr = fake_regs.IMR;

	rc = i2s_dw_trigger(&test_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	zassert_equal(rc, 0, "rx drop cleanup failed: %d", rc);

	zassert_equal(dir_after_rx_start, I2S_DIR_RX, "RX START did not point dir at RX");
	zassert_equal(imr_after_isr & I2S_IMR_RXDAM_Msk,
	              0U,
	              "the restarted RX's RXDA was masked as unserviced (IMR=0x%08x)",
	              imr_after_isr);
}

/*
 * (w) alp-sdk issue #2205: a RUNNING TX pre-empted by an RX START goes to
 * I2S_STATE_ERROR through the #2149 keep-clock exit, instead of staying
 * "RUNNING" with nothing draining its queue (on the E8 the old
 * i2s_tx_channel_disable() stopped nothing, and dir had moved to RX). The
 * #2150 phase 1 / #2171 contract holds throughout: CER.CLKEN stays set
 * through the RX start AND the RX teardown, because the park arms
 * clk_restart_skip_ok and tx_clock_is_live() honours it. Then the
 * backend's recovery: write() gets -EIO, PREPARE, write, START -- which
 * restarts TX on the kept clock without reprogramming it.
 *
 * MUTATION-PROVEN, three ways:
 *   - deleting the tx_stream_park_keep_clock() call from rx_stream_start()
 *     turns this RED (TX stays RUNNING, ITER.TXEN stays set);
 *   - parking with the clock gated (tx_stream_disable() in its place)
 *     turns it RED (flag not armed; the unconditional i2s_clock_enable()
 *     later in rx_stream_start() turns CER back on, so the loss shows at
 *     the RX stop instead) -- and test (h) with it;
 *   - parking without arming clk_restart_skip_ok
 *     (tx_stream_disable_ex(..., true) in its place) turns it RED the same
 *     way: CER.CLKEN is gated by the RX stop -- the #2171 contract broken
 *     -- and test (h) goes RED too;
 *   - deleting the `if (clk_needs_reprogram)` guard around
 *     i2s_configure_clock() in tx_stream_start() turns it RED on the CCR
 *     sentinel (set_rate() alone does not see a live CCR rewrite).
 */
ZTEST(i2s_dw_underrun, test_rx_start_preempts_running_tx_keeps_clock)
{
	void        *block;
	int32_t      tx_state_after_rx_start;
	uint32_t     iter_after_rx_start;
	bool         clk_on_after_rx_start;
	bool         flag_after_rx_start;
	bool         clk_on_after_rx_stop;
	int32_t      tx_state_after_retry;
	enum i2s_dir dir_after_retry;
	uint32_t     ccr_after_retry;
	int          set_rate_delta;
	int          write_rc;
	int          before;
	int          rc;

	tx_configure(16000);
	tx_write_block();
	tx_start();
	zassert_equal(test_data.tx.state, I2S_STATE_RUNNING, "tx not RUNNING after start");

	rx_configure(16000);
	rx_start();
	tx_state_after_rx_start = test_data.tx.state;
	iter_after_rx_start     = fake_regs.ITER;
	clk_on_after_rx_start   = (fake_regs.CER & I2S_CER_CLKEN_Msk) != 0;
	flag_after_rx_start     = test_data.tx.clk_restart_skip_ok;

	rc = i2s_dw_trigger(&test_dev, I2S_DIR_RX, I2S_TRIGGER_STOP);
	zassert_equal(rc, 0, "rx stop failed: %d", rc);
	clk_on_after_rx_stop = (fake_regs.CER & I2S_CER_CLKEN_Msk) != 0;

	/* The app's next write(): -EIO, the block is not queued. */
	rc = k_mem_slab_alloc(&test_tx_slab, &block, K_NO_WAIT);
	zassert_equal(rc, 0, "tx slab alloc failed: %d", rc);
	write_rc = i2s_dw_write(&test_dev, block, TEST_BLOCK_BYTES);
	k_mem_slab_free(&test_tx_slab, block);

	zassert_equal(tx_state_after_rx_start,
	              I2S_STATE_ERROR,
	              "RUNNING TX not pre-empted to ERROR by the RX start (state %d)",
	              tx_state_after_rx_start);
	zassert_equal(
	    iter_after_rx_start & I2S_ITER_TXEN_Msk, 0U, "ITER.TXEN left set by the RX start");
	zassert_true(clk_on_after_rx_start, "the RX start pre-emption gated CER.CLKEN");
	zassert_true(flag_after_rx_start, "the pre-emption did not arm clk_restart_skip_ok");
	zassert_true(clk_on_after_rx_stop,
	             "RX teardown gated CER.CLKEN while the pre-empted TX was still live");
	zassert_equal(write_rc, -EIO, "write() to the pre-empted TX did not fail -EIO: %d", write_rc);

	/* The backend's ERROR->PREPARE->START retry. The sentinel proves the
	 * restart skips the live CCR rewrite too, not only set_rate(). */
	tx_prepare();
	tx_write_block();
	before        = fake_set_rate_calls;
	fake_regs.CCR = 0xDEADBEEFU;
	tx_start();
	tx_state_after_retry = test_data.tx.state;
	dir_after_retry      = test_data.dir;
	ccr_after_retry      = fake_regs.CCR;
	set_rate_delta       = fake_set_rate_calls - before;

	/* Cleanup before the assertions so it still runs if one aborts. */
	rc = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	zassert_equal(rc, 0, "tx drop cleanup failed: %d", rc);

	zassert_equal(tx_state_after_retry, I2S_STATE_RUNNING, "tx not RUNNING after the retry");
	zassert_equal(dir_after_retry, I2S_DIR_TX, "tx restart did not point dir at TX");
	zassert_equal(set_rate_delta, 0, "tx restart reprogrammed the kept clock");
	zassert_equal(ccr_after_retry, 0xDEADBEEFU, "tx restart rewrote CCR on the kept clock");
}

/*
 * (x) alp-sdk issue #2205: the mirror of (w) -- a RUNNING RX pre-empted by
 * a TX START goes to I2S_STATE_ERROR through rx_stream_disable() (block
 * disabled, its in-flight buffer freed), and the TX start still ends with
 * CER.CLKEN set.
 *
 * MUTATION-PROVEN: deleting the RX pre-emption from tx_stream_start()
 * turns this RED (RX stays RUNNING, IRER.RXEN stays set).
 */
ZTEST(i2s_dw_underrun, test_tx_start_preempts_running_rx)
{
	int32_t  rx_state_after_tx_start;
	uint32_t irer_after_tx_start;
	void    *rx_block_after_tx_start;
	int      rc;

	rx_configure(16000);
	rx_start();
	tx_configure(16000);
	tx_write_block();
	tx_start();
	rx_state_after_tx_start = test_data.rx.state;
	irer_after_tx_start     = fake_regs.IRER;
	rx_block_after_tx_start = test_data.rx.mem_block;

	zassert_true((fake_regs.CER & I2S_CER_CLKEN_Msk) != 0, "TX start left CER.CLKEN clear");

	rc = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	zassert_equal(rc, 0, "tx drop cleanup failed: %d", rc);
	rc = i2s_dw_trigger(&test_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	zassert_equal(rc, 0, "rx drop cleanup failed: %d", rc);

	zassert_equal(rx_state_after_tx_start,
	              I2S_STATE_ERROR,
	              "RUNNING RX not pre-empted to ERROR by the TX start (state %d)",
	              rx_state_after_tx_start);
	zassert_equal(
	    irer_after_tx_start & I2S_IRER_RXEN_Msk, 0U, "IRER.RXEN left set by the TX start");
	zassert_is_null(rx_block_after_tx_start, "pre-empted RX kept its in-flight block");
}

/*
 * (y) alp-sdk issue #2205: the channel-enable helpers read-modify-write.
 * A plain write of 0x1 zeroed the E8's per-slot enables in bits 8-23 --
 * measured on E1M-AEN803 serial 2026W36-0002: RER went 0x00FFFF01 -> 0x00000001 at the
 * RX start.
 *
 * MUTATION-PROVEN: restoring the plain `TER = 0x1` / `RER = 0x1` writes
 * turns this RED on both assertions.
 */
ZTEST(i2s_dw_underrun, test_channel_enable_preserves_slot_enables)
{
	uint32_t ter_after_tx_start;
	uint32_t rer_after_rx_start;
	int      rc;

	tx_configure(16000);
	tx_write_block();
	tx_start();
	ter_after_tx_start = fake_ter();
	rc                 = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	zassert_equal(rc, 0, "tx drop failed: %d", rc);

	rx_configure(16000);
	rx_start();
	rer_after_rx_start = fake_rer();
	rc                 = i2s_dw_trigger(&test_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
	zassert_equal(rc, 0, "rx drop failed: %d", rc);

	zassert_equal(ter_after_tx_start,
	              E8_TER_RESET,
	              "TX start clobbered the TX slot enables (TER=0x%08x)",
	              ter_after_tx_start);
	zassert_equal(rer_after_rx_start,
	              E8_RER_RESET,
	              "RX start clobbered the RX slot enables (RER=0x%08x)",
	              rer_after_rx_start);
}

/* ---------------------------------------------------------------------
 * alp-sdk issue #2205: one helper thread that blocks in i2s_dw_read() or
 * i2s_dw_write() while the test thread pre-empts its stream. The backend
 * (src/backends/i2s/zephyr_drv.c) configures SYS_FOREVER_MS, so a waiter
 * that is never released hangs for good -- and alp_i2s_close() waits for
 * it before its DROP could reset the semaphore.
 * ------------------------------------------------------------------ */
#define WAITER_STACK_SIZE 4096

K_THREAD_STACK_DEFINE(waiter_stack, WAITER_STACK_SIZE);
static struct k_thread waiter_thread;
static volatile int    waiter_rc;
static volatile bool   waiter_done;

/* Room for 1 in-flight + CONFIG_I2S_DW_TX_BLOCK_COUNT queued + 1 waiting. */
K_MEM_SLAB_DEFINE_STATIC(test_tx_big_slab, TEST_BLOCK_BYTES, 8, 4);

static void reader_entry(void *p1, void *p2, void *p3)
{
	void  *block = NULL;
	size_t size  = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	waiter_rc = i2s_dw_read(&test_dev, &block, &size);
	if (waiter_rc == 0) {
		k_mem_slab_free(&test_rx_slab, block);
	}
	waiter_done = true;
}

static int big_write(void)
{
	void *block;
	int   rc = k_mem_slab_alloc(&test_tx_big_slab, &block, K_NO_WAIT);

	if (rc == 0) {
		rc = i2s_dw_write(&test_dev, block, TEST_BLOCK_BYTES);
		if (rc != 0) {
			k_mem_slab_free(&test_tx_big_slab, block);
		}
	}
	return rc;
}

static void writer_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	waiter_rc   = big_write();
	waiter_done = true;
}

/* Start the waiter and give it time to block. */
static void start_waiter(k_thread_entry_t entry)
{
	waiter_done = false;
	waiter_rc   = 1;
	k_thread_create(&waiter_thread,
	                waiter_stack,
	                K_THREAD_STACK_SIZEOF(waiter_stack),
	                entry,
	                NULL,
	                NULL,
	                NULL,
	                K_PRIO_PREEMPT(1),
	                0,
	                K_NO_WAIT);
	k_sleep(K_MSEC(20));
}

/* True if the waiter returned; otherwise it is aborted so it cannot leak
 * into later cases. */
static bool join_waiter(void)
{
	if (k_thread_join(&waiter_thread, K_MSEC(500)) == 0) {
		return true;
	}
	k_thread_abort(&waiter_thread);
	return false;
}

/*
 * (z) alp-sdk issue #2205: a reader blocked in i2s_dw_read() when a TX
 * START pre-empts its RX stream is released with -EIO. It used to wait on
 * rx.sem forever (nothing gives it once dir is I2S_DIR_TX), so the app's
 * alp_i2s_close() on that RX handle deadlocked waiting for the read.
 *
 * MUTATION-PROVEN: deleting the k_sem_give(&dev_data->rx.sem) from
 * tx_stream_start()'s pre-emption turns this RED (reader never returns).
 */
ZTEST(i2s_dw_underrun, test_tx_start_preempting_rx_releases_blocked_reader)
{
	struct i2s_config cfg = make_cfg(16000, &test_rx_slab);
	bool              blocked_before;
	bool              released;
	int               rc;

	cfg.timeout = SYS_FOREVER_MS;
	rc          = i2s_dw_configure(&test_dev, I2S_DIR_RX, &cfg);
	zassert_equal(rc, 0, "rx configure failed: %d", rc);
	rx_start();

	start_waiter(reader_entry);
	blocked_before = !waiter_done;

	tx_configure(16000);
	tx_write_block();
	tx_start();
	released = join_waiter();

	zassert_true(blocked_before, "the reader never blocked -- this case proves nothing");
	zassert_true(released, "a reader blocked on the pre-empted RX stream was never released");
	zassert_equal(waiter_rc, -EIO, "released reader did not get -EIO: %d", waiter_rc);
}

/*
 * (aa) alp-sdk issue #2205: the mirror of (z). A writer blocked in
 * i2s_dw_write() on a full TX queue when an RX START pre-empts TX is
 * released with -EIO, instead of waiting on tx.sem forever or, once woken,
 * queueing into the parked stream and reporting success.
 *
 * MUTATION-PROVEN, two ways: deleting the k_sem_give(&dev_data->tx.sem)
 * from rx_stream_start()'s pre-emption turns this RED (writer never
 * returns); deleting i2s_dw_write()'s post-wait ERROR check turns it RED
 * (writer returns 0).
 */
ZTEST(i2s_dw_underrun, test_rx_start_preempting_tx_releases_blocked_writer)
{
	struct i2s_config cfg = make_cfg(16000, &test_tx_big_slab);
	bool              blocked_before;
	bool              released;
	int               rc;

	cfg.timeout = SYS_FOREVER_MS;
	rc          = i2s_dw_configure(&test_dev, I2S_DIR_TX, &cfg);
	zassert_equal(rc, 0, "tx configure failed: %d", rc);
	rc = big_write();
	zassert_equal(rc, 0, "tx write failed: %d", rc);
	tx_start();
	for (int i = 0; i < CONFIG_I2S_DW_TX_BLOCK_COUNT; i++) {
		rc = big_write();
		zassert_equal(rc, 0, "tx fill write %d failed: %d", i, rc);
	}
	zassert_equal(k_sem_count_get(&test_data.tx.sem), 0U, "TX queue not full");

	start_waiter(writer_entry);
	blocked_before = !waiter_done;

	rx_configure(16000);
	rx_start();
	released = join_waiter();

	zassert_true(blocked_before, "the writer never blocked -- this case proves nothing");
	zassert_true(released, "a writer blocked on the pre-empted TX stream was never released");
	zassert_equal(waiter_rc, -EIO, "released writer did not get -EIO: %d", waiter_rc);
}

/*
 * (ab) alp-sdk issue #2205: a TX START that fails (empty queue, -ENOMEM)
 * must not pre-empt RX -- the pre-emption sits after queue_get().
 *
 * MUTATION-PROVEN: moving tx_stream_start()'s RX pre-emption above
 * queue_get() turns this RED (RX parked in ERROR, IRER.RXEN clear).
 */
ZTEST(i2s_dw_underrun, test_failed_tx_start_leaves_rx_running)
{
	int32_t  rx_state_after;
	uint32_t irer_after;
	int      rc;

	rx_configure(16000);
	rx_start();
	tx_configure(16000);

	rc             = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	rx_state_after = test_data.rx.state;
	irer_after     = fake_regs.IRER;

	zassert_equal(rc, -ENOMEM, "TX START on an empty queue did not fail -ENOMEM: %d", rc);
	zassert_equal(rx_state_after,
	              I2S_STATE_RUNNING,
	              "a failed TX START pre-empted RX (state %d)",
	              rx_state_after);
	zassert_equal(
	    irer_after & I2S_IRER_RXEN_Msk, I2S_IRER_RXEN_Msk, "a failed TX START cleared IRER.RXEN");
}

/*
 * (ac) alp-sdk issue #2205: the backend's TX retry pre-empting a RUNNING
 * RX must never gate the bit clock, even for an instant. The flag armed
 * when RX parked TX keeps tx_clock_is_live() true inside the RX teardown
 * only because tx_stream_start() pre-empts RX BEFORE it consumes that
 * flag. The other order gates CER.CLKEN in rx_stream_disable() and turns
 * it back on a few lines later: 1 -> 0 -> 1, the bit-clock loss a codec
 * latches SHUTDOWN on (#2149). The final register value cannot show that,
 * so this counts every clear.
 *
 * MUTATION-PROVEN: moving the RX pre-emption below
 * `stream->clk_restart_skip_ok = false;` turns this RED (one clear).
 */
ZTEST(i2s_dw_underrun, test_tx_retry_preempting_rx_never_gates_clock)
{
	void   *block;
	int32_t rx_state_after;
	bool    clk_on_after;
	int     clears;
	int     write_rc;
	int     rc;

	tx_configure(16000);
	tx_write_block();
	tx_start();
	rx_configure(16000);
	rx_start();

	rc = k_mem_slab_alloc(&test_tx_slab, &block, K_NO_WAIT);
	zassert_equal(rc, 0, "tx slab alloc failed: %d", rc);
	write_rc = i2s_dw_write(&test_dev, block, TEST_BLOCK_BYTES);
	k_mem_slab_free(&test_tx_slab, block);
	zassert_equal(write_rc, -EIO, "write to the parked TX did not fail -EIO: %d", write_rc);

	tx_prepare();
	tx_write_block();
	fake_cer_clears = 0;
	tx_start();
	clears         = fake_cer_clears;
	rx_state_after = test_data.rx.state;
	clk_on_after   = (fake_regs.CER & I2S_CER_CLKEN_Msk) != 0;

	zassert_equal(
	    clears, 0, "the TX retry gated CER.CLKEN %d time(s) while pre-empting RX", clears);
	zassert_true(clk_on_after, "CER.CLKEN clear after the TX retry");
	zassert_equal(rx_state_after,
	              I2S_STATE_ERROR,
	              "the TX retry did not pre-empt the RUNNING RX (state %d)",
	              rx_state_after);
}

/*
 * (ad) alp-sdk issue #2205, found on the bench: after an RX START parks a
 * RUNNING TX in ERROR, the backend's stop() tries DRAIN before falling
 * back to DROP, and the driver refused it with an error log --
 * "DRAIN trigger: invalid state" on every clean teardown. DRAIN and STOP
 * from ERROR must still be refused (-EIO; the stream stays in ERROR for
 * DROP or PREPARE), but without an error-level log.
 *
 * MUTATION-PROVEN: logging the ERROR refusal with LOG_ERR again, in DRAIN
 * or in STOP, turns this RED (one error log); letting DRAIN accept ERROR
 * turns it RED (rc 0).
 */
ZTEST(i2s_dw_underrun, test_drain_stop_in_error_refused_quietly)
{
	int32_t state_after;
	int     drain_rc;
	int     stop_rc;
	int     errs;

	tx_configure(16000);
	tx_write_block();
	tx_start();
	rx_configure(16000);
	rx_start();
	zassert_equal(test_data.tx.state, I2S_STATE_ERROR, "RX START did not park TX");

	fake_log_errs = 0;
	drain_rc      = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	stop_rc       = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_STOP);
	errs          = fake_log_errs;
	state_after   = test_data.tx.state;

	zassert_equal(drain_rc, -EIO, "DRAIN from ERROR was not refused: %d", drain_rc);
	zassert_equal(stop_rc, -EIO, "STOP from ERROR was not refused: %d", stop_rc);
	zassert_equal(errs, 0, "refusing DRAIN/STOP from ERROR logged %d error(s)", errs);
	zassert_equal(state_after, I2S_STATE_ERROR, "refused triggers moved TX out of ERROR");
}

/*
 * (ae) alp-sdk issue #2205: the other half of (ad) -- a DRAIN or STOP from
 * READY (never started) is a caller bug and still logs at error level.
 *
 * MUTATION-PROVEN: making the DRAIN refusal always quiet turns this RED.
 */
ZTEST(i2s_dw_underrun, test_drain_stop_in_ready_still_logs_error)
{
	int drain_rc;
	int stop_rc;
	int errs;

	tx_configure(16000);

	fake_log_errs = 0;
	drain_rc      = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	stop_rc       = i2s_dw_trigger(&test_dev, I2S_DIR_TX, I2S_TRIGGER_STOP);
	errs          = fake_log_errs;

	zassert_equal(drain_rc, -EIO, "DRAIN from READY was not refused: %d", drain_rc);
	zassert_equal(stop_rc, -EIO, "STOP from READY was not refused: %d", stop_rc);
	zassert_equal(errs, 2, "DRAIN/STOP from READY logged %d error(s), not 2", errs);
}
