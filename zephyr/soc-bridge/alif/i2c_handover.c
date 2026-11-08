/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ADR-0017-ADJACENT glue (no upstream equivalent for a
 * controller shared between two cores of one SoC).
 *
 * Alif Ensemble: hand one DesignWare I2C controller from the core that
 * configures a peripheral over it at boot to the core that owns the bus
 * afterwards (binding: alp,i2c-handover.yaml; flag protocol: i2c_handover.h).
 *
 * The Ensemble has ONE I2C1 block (0x49011000) that both M55 cores can
 * address, each running its own i2c_dw driver. Two drivers on one bus is a
 * contention hazard whether or not either is idle: this core's ISR would also
 * run on the other core's transfers and clear INTR_MASK / status under it
 * (the same hazard the Trace Runner's INA236 reader documents for I2C2), and a
 * pad-level bus recovery on one core in the middle of the other's transaction
 * corrupts it. So the handover is explicit:
 *
 *   release: first thing at boot (PRE_KERNEL_1 0): snapshot the record and clear a
 *            release nobody took (alp_i2c_handover_boot_snapshot()); then, ahead of the I2C
 *            drivers (POST_KERNEL 49), decide whether this is a WARM boot (see below). At the
 *            end of APPLICATION init, mask this core's NVIC line for the controller, clear IC_ENABLE
 *            (0x6C), poll IC_ENABLE_STATUS (0x9C) until idle, then publish. Runs
 *            at the end of APPLICATION init, after the drivers that used the
 *            bus (the SN65DSI83 display bridge initialises at
 *            CONFIG_APPLICATION_INIT_PRIORITY). A controller that will not go
 *            idle is still released, as DIRTY, with a warning: the other core
 *            then runs its own bus recovery instead of waiting forever. A bus
 *            node that is DISABLED in this image has nothing to stop and is
 *            released at once.
 *   acquire: POST_KERNEL priority 0 -- before any driver (the i2c_dw instance is
 *            priority 40) -- waits for the release, reports on the console every
 *            20 s while it is missing, never touches the bus meanwhile, and
 *            consumes the release once it has it. With an alive-address it then starts a
 *            10 ms timer that advances that word (the release side's warm-boot detection
 *            reads it): started only AFTER the bus is taken, from a timer so a slow main
 *            loop cannot starve it.
 *
 * WARM BOOT (release side, optional alive-address): this core alone was reset while the
 * acquiring core runs and owns the bus. The record reads "taken" and the acquiring core's
 * liveness word moves (alp_i2c_handover_is_warm()), so the bus is NOT ours: the controller
 * is never initialised (the bus node carries zephyr,deferred-init; a cold boot
 * device_init()s it from here, a warm one never does), its NVIC line stays masked, the
 * state word is left alone and nothing is released. Initialising it would reset the
 * controller under the other core's transfer and leave it wedged (bench, HE-only reset
 * under a running camera). A drivers that would have used the bus asks
 * alp_i2c_handover_warm_boot().
 *
 * An acquire image restarted alone, after it took the release, waits for one
 * that will not come again: restart both, or write the clean magic to state with
 * a fresh nonce. A release-side reset while the acquire side is already running
 * reconfigures the bus under it: reset both.
 */
#define DT_DRV_COMPAT alp_i2c_handover

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/toolchain.h>

#define ALP_I2C_HANDOVER_BARRIER() barrier_dsync_fence_full()
#include "i2c_handover.h"

#define DW_IC_ENABLE        0x6Cu /* DesignWare APB I2C */
#define DW_IC_ENABLE_STATUS 0x9Cu
#define DW_IC_EN_BIT        BIT(0)

#define I2C_HANDOVER_BOOT_PRIO     49 /* ahead of CONFIG_I2C_INIT_PRIORITY (50) */
#define I2C_HANDOVER_RELEASE_PRIO 99 /* last of APPLICATION: after every bus user's init */
#define I2C_HANDOVER_WAIT_STEP_MS 5
#define I2C_HANDOVER_REPORT_MS    20000
BUILD_ASSERT(ALP_I2C_HANDOVER_ALIVE_PERIOD_MS * 1000u * 2u < ALP_I2C_HANDOVER_ALIVE_WINDOW_US,
             "the liveness timer must tick at least twice inside the sampling window");

/* Release side: the record read "taken" at PRE_KERNEL_1 (before anything could change it). */
static bool __maybe_unused i2c_handover_taken_at_boot;
static bool i2c_handover_warm;

bool alp_i2c_handover_warm_boot(void)
{
	return i2c_handover_warm;
}

#define HANDOVER_WORDS(inst) ((alp_i2c_handover_t *)(uintptr_t)DT_INST_PROP(inst, flag_address))

/* Stop the DesignWare controller at `base` and mask its line on this core. True when it went idle. */
static bool __maybe_unused i2c_handover_stop(uintptr_t base, unsigned int irq)
{
	irq_disable(irq);
	sys_write32(0u, base + DW_IC_ENABLE);
	for (int i = 0; i < 100 && (sys_read32(base + DW_IC_ENABLE_STATUS) & DW_IC_EN_BIT); i++) {
		k_busy_wait(100);
	}
	return !(sys_read32(base + DW_IC_ENABLE_STATUS) & DW_IC_EN_BIT);
}

static void __maybe_unused i2c_handover_delay_us(uint32_t us)
{
	k_busy_wait(us);
}

static void __maybe_unused i2c_handover_release(alp_i2c_handover_t *w, bool idle)
{
	alp_i2c_handover_publish(w, k_cycle_get_32() ^ (uint32_t)k_uptime_get(), !idle);
	if (idle) {
		printk("i2c-handover: bus released\n");
	} else {
		printk("i2c-handover: controller did not go idle -- released DIRTY, the other core "
		       "runs its bus recovery\n");
	}
}

static void __maybe_unused i2c_handover_acquire(alp_i2c_handover_t *w)
{
	int64_t t0 = k_uptime_get();
	int     r;

	printk("i2c-handover: waiting for the other core to release the bus\n");
	while ((r = alp_i2c_handover_try_acquire(w)) == ALP_I2C_HANDOVER_NOT_YET) {
		if (k_uptime_get() - t0 >= I2C_HANDOVER_REPORT_MS) {
			printk("i2c-handover: bus NOT released after 20 s -- not touching it, still "
			       "waiting\n");
			t0 = k_uptime_get();
		}
		k_msleep(I2C_HANDOVER_WAIT_STEP_MS);
	}
	printk(r == ALP_I2C_HANDOVER_TAKEN
	           ? "i2c-handover: bus acquired\n"
	           : "i2c-handover: bus acquired DIRTY -- recover it before use\n");
}

#define HANDOVER_ALIVE_WORD(inst) \
	((volatile uint32_t *)(uintptr_t)DT_INST_PROP(inst, alive_address))

#define HANDOVER_ALIVE(inst) \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, alive_address), (HANDOVER_ALIVE_WORD(inst)), (NULL))

/* The bus node is initialised by hand on a cold boot, never on a warm one. */
#define HANDOVER_BUS_DEFERRED(inst) DT_PROP_OR(DT_INST_PHANDLE(inst, bus), zephyr_deferred_init, 0)

/* Cold boot: bring the deferred bus up by hand (a bus that is not deferred was already
 * initialised by its own driver, nothing to do here). */
#define HANDOVER_COLD_BUS_INIT(inst) \
	COND_CODE_1(DT_NODE_HAS_STATUS(DT_INST_PHANDLE(inst, bus), okay), \
	            (if (HANDOVER_BUS_DEFERRED(inst)) { \
		            int r = device_init(DEVICE_DT_GET(DT_INST_PHANDLE(inst, bus))); \
\
		            if (r != 0) { \
			            printk("i2c-handover: bus init failed (%d)\n", r); \
		            } \
	            }), \
	            ())

#define HANDOVER_RELEASE(inst) \
	BUILD_ASSERT(DT_INST_NODE_HAS_PROP(inst, bus), "role release needs a bus phandle"); \
	BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_INST_PHANDLE(inst, bus), snps_designware_i2c), \
	             "bus must be a snps,designware-i2c controller"); \
	BUILD_ASSERT(CONFIG_APPLICATION_INIT_PRIORITY < I2C_HANDOVER_RELEASE_PRIO, \
	             "the release must run after the application-priority drivers that use the bus"); \
	BUILD_ASSERT(!DT_INST_NODE_HAS_PROP(inst, alive_address) || HANDOVER_BUS_DEFERRED(inst), \
	             "alive-address (warm-boot detection) needs the bus node to carry " \
	             "zephyr,deferred-init, or a warm boot would initialise the shared controller"); \
	COND_CODE_1(HANDOVER_BUS_DEFERRED(inst), \
	            (BUILD_ASSERT(CONFIG_I2C_INIT_PRIORITY >= I2C_HANDOVER_BOOT_PRIO, \
	                          "the boot-time sample must run before the I2C drivers' init " \
	                          "priority");), \
	            ()) \
	static int i2c_handover_snapshot_##inst(void) \
	{ \
		i2c_handover_taken_at_boot = alp_i2c_handover_boot_snapshot(HANDOVER_WORDS(inst)); \
		return 0; \
	} \
	SYS_INIT(i2c_handover_snapshot_##inst, PRE_KERNEL_1, 0); \
	static int i2c_handover_boot_##inst(void) \
	{ \
		i2c_handover_warm = alp_i2c_handover_sample_warm(HANDOVER_WORDS(inst), \
		                                                 i2c_handover_taken_at_boot, \
		                                                 HANDOVER_ALIVE(inst), \
		                                                 i2c_handover_delay_us, \
		                                                 ALP_I2C_HANDOVER_ALIVE_STEP_US, \
		                                                 ALP_I2C_HANDOVER_ALIVE_WINDOW_US); \
		if (i2c_handover_warm) { \
			printk("i2c-handover: warm boot, the other core owns the bus -- not touching " \
			       "it\n"); \
			return 0; \
		} \
		alp_i2c_handover_reset(HANDOVER_WORDS(inst)); \
		HANDOVER_COLD_BUS_INIT(inst) \
		return 0; \
	} \
	SYS_INIT(i2c_handover_boot_##inst, POST_KERNEL, I2C_HANDOVER_BOOT_PRIO); \
	static int i2c_handover_release_##inst(void) \
	{ \
		bool idle = true; \
\
		if (i2c_handover_warm) { \
			return 0; \
		} \
		COND_CODE_1(DT_NODE_HAS_STATUS(DT_INST_PHANDLE(inst, bus), okay), \
		            (idle = i2c_handover_stop(DT_REG_ADDR(DT_INST_PHANDLE(inst, bus)), \
		                                      DT_IRQN(DT_INST_PHANDLE(inst, bus)));), \
		            ()) \
		i2c_handover_release(HANDOVER_WORDS(inst), idle); \
		return 0; \
	} \
	SYS_INIT(i2c_handover_release_##inst, APPLICATION, I2C_HANDOVER_RELEASE_PRIO);

#define HANDOVER_ACQUIRE(inst) \
	COND_CODE_1( \
	    DT_INST_NODE_HAS_PROP(inst, alive_address), \
	    (static void i2c_handover_alive_tick_##inst(struct k_timer *t) { \
		    ARG_UNUSED(t); \
		    (*HANDOVER_ALIVE_WORD(inst))++; \
	    } K_TIMER_DEFINE(i2c_handover_alive_timer_##inst, i2c_handover_alive_tick_##inst, NULL);), \
	    ()) \
	static int i2c_handover_acquire_##inst(void) \
	{ \
		i2c_handover_acquire(HANDOVER_WORDS(inst)); \
		/* Liveness starts only now, with the bus in hand. */ \
		COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, alive_address), \
		            (k_timer_start(&i2c_handover_alive_timer_##inst, \
		                           K_MSEC(ALP_I2C_HANDOVER_ALIVE_PERIOD_MS), \
		                           K_MSEC(ALP_I2C_HANDOVER_ALIVE_PERIOD_MS));), \
		            ()) \
		return 0; \
	} \
	SYS_INIT(i2c_handover_acquire_##inst, POST_KERNEL, 0);

#define HANDOVER_INST(inst) \
	COND_CODE_1(DT_INST_ENUM_HAS_VALUE(inst, role, release), \
	            (HANDOVER_RELEASE(inst)), \
	            (HANDOVER_ACQUIRE(inst)))

DT_INST_FOREACH_STATUS_OKAY(HANDOVER_INST)
