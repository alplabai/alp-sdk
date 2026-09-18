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
static uint32_t rer_at_irq_config;
static uint32_t ter_at_irq_config;

static void capture_irq_config(const struct device *dev)
{
	ARG_UNUSED(dev);

	irq_config_calls++;
	imr_at_irq_config  = fake_regs.IMR;
	ier_at_irq_config  = fake_regs.IER;
	irer_at_irq_config = fake_regs.IRER;
	iter_at_irq_config = fake_regs.ITER;
	rer_at_irq_config  = fake_regs.RER;
	ter_at_irq_config  = fake_regs.TER;
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

	memset(&fake_regs, 0, sizeof(fake_regs));
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
	rer_at_irq_config  = 0;
	ter_at_irq_config  = 0;

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

ZTEST_SUITE(i2s_dw_underrun, NULL, NULL, dw_test_before, NULL, NULL);

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
 * (a) After a queue-empty underrun: state ERROR, CER bit 0 == 1, TER
 *     bit 0 == 0, TX interrupt disabled.
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
	zassert_true((fake_regs.TER & I2S_TER_TXCHEN_Msk) == 0, "TER.TXCHEN still set after underrun");
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
 * rx_stream_disable()) must NOT gate the shared bit clock while TX is
 * still RUNNING on it. Reverting rx_stream_disable()'s `if (!tx_live)`
 * guard back to an unconditional i2s_clock_disable() turns this RED --
 * CER.CLKEN reads 0 after the RX stop even though TX is still RUNNING,
 * which is the exact silent-amp exposure #2150 phase 1 closes.
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

	zassert_true(clk_on_after_rx_stop, "RX stop gated the shared clock while TX was still RUNNING");
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

	tx_configure(16000);
	tx_write_block();
	tx_start();

	rx_configure(48000);
	before                        = fake_set_rate_calls;
	rx_slab_free_before           = k_mem_slab_num_free_get(&test_rx_slab);
	rx_start_rc                   = i2s_dw_trigger(&test_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	set_rate_calls_after_rx_start = fake_set_rate_calls;
	rx_slab_free_after            = k_mem_slab_num_free_get(&test_rx_slab);

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
 * NOT in the SYSRESETREQ reset domain -- measured on e1m-aen-evk-03, CER
 * and TER both survived a J-Link `loadbin`'s implicit SYSRESETREQ at
 * 0x00000001 and only a RESETPIN reset cleared them to 0x00000000 -- so a
 * fresh image inherits the previous one's IER/IMR/IRER/ITER/RER/TER. The
 * old order armed the NVIC first and masked two statements later, with
 * dev_data->dir fresh from BSS as 0 (== I2S_DIR_RX) and no stream
 * configured at all.
 *
 * The assertions read the state captured INSIDE irq_config(), not the
 * register file after init returns -- that is the only instant the
 * ordering is observable.
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
	zassert_equal(
	    rer_at_irq_config & I2S_RER_RXCHEN_Msk, 0U, "NVIC armed with RER.RXCHEN still set");
	zassert_equal(
	    ter_at_irq_config & I2S_TER_TXCHEN_Msk, 0U, "NVIC armed with TER.TXCHEN still set");

	/* The #2149/#2150 invariant is unchanged: CER.CLKEN reads 1 from
	 * i2s_dw_initialize() onward -- it is just set a few statements later
	 * now, by the i2s_enable_controller() call this reordering moved
	 * below irq_config(). */
	zassert_true((fake_regs.CER & I2S_CER_CLKEN_Msk) != 0,
	             "i2s_dw_initialize() left CER.CLKEN clear");
}
