/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif Ensemble: hand one DesignWare I2C controller from the core that
 * configures a peripheral over it at boot to the core that owns the bus
 * afterwards (binding: alp,i2c-handover.yaml).
 *
 * The Ensemble has ONE I2C1 block (0x49011000) that both M55 cores can
 * address, each running its own i2c_dw driver. Two drivers on one bus is a
 * contention hazard whether or not either is idle: this core's ISR would also
 * run on the other core's transfers and clear INTR_MASK / status under it
 * (the same hazard the Trace Runner's INA236 reader documents for I2C2), and a
 * pad-level bus recovery on one core in the middle of the other's transaction
 * corrupts it. So the handover is explicit:
 *
 *   release: mask this core's NVIC line for the controller, clear IC_ENABLE
 *            (0x6C), poll IC_ENABLE_STATUS (0x9C) until idle, then write the
 *            flag. Runs at the end of APPLICATION init, after the drivers that
 *            used the bus (the SN65DSI83 display bridge initialises at
 *            CONFIG_APPLICATION_INIT_PRIORITY). The flag is also cleared first
 *            thing at boot: SRAM0 survives a warm reset.
 *   acquire: POST_KERNEL priority 0 -- before any driver (the i2c_dw instance is
 *            priority 40) -- waits for the flag, reports on the console every
 *            20 s while it is missing, never touches the bus meanwhile, and
 *            clears the flag when it has it.
 *
 * An acquire image restarted alone, after it already consumed the flag, waits
 * for a release that will not come again: restart both images, or write
 * 0x31433249 to the flag address first.
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

#define I2C_HANDOVER_MAGIC 0x31433249u /* 'I2C1' */

#define DW_IC_ENABLE        0x6Cu /* DesignWare APB I2C */
#define DW_IC_ENABLE_STATUS 0x9Cu
#define DW_IC_EN_BIT        BIT(0)

#define I2C_HANDOVER_RELEASE_PRIO 99 /* last of APPLICATION: after every bus user's init */
#define I2C_HANDOVER_WAIT_STEP_MS 5
#define I2C_HANDOVER_REPORT_MS    20000

#define HANDOVER_FLAG(inst) ((volatile uint32_t *)(uintptr_t)DT_INST_PROP(inst, flag_address))

/* ---- release ---------------------------------------------------------- */
#define HANDOVER_RELEASE_BODY(inst) \
	({ \
		bool ok = true; \
\
		COND_CODE_1( \
		    DT_NODE_HAS_STATUS(DT_INST_PHANDLE(inst, bus), okay), \
		    (uintptr_t base = DT_REG_ADDR(DT_INST_PHANDLE(inst, bus)); \
\
		     irq_disable(DT_IRQN(DT_INST_PHANDLE(inst, bus))); \
		     sys_write32(0u, base + DW_IC_ENABLE); \
		     for (int i = 0; i < 100 && (sys_read32(base + DW_IC_ENABLE_STATUS) & DW_IC_EN_BIT); \
		          i++) { k_busy_wait(100); } ok = \
		         !(sys_read32(base + DW_IC_ENABLE_STATUS) & DW_IC_EN_BIT);), \
		    ()) \
		ok; \
	})

#define HANDOVER_RELEASE(inst) \
	static int i2c_handover_clear_##inst(void) \
	{ \
		*HANDOVER_FLAG(inst) = 0u; \
		return 0; \
	} \
	SYS_INIT(i2c_handover_clear_##inst, PRE_KERNEL_1, 0); \
	static int i2c_handover_release_##inst(void) \
	{ \
		if (!HANDOVER_RELEASE_BODY(inst)) { \
			printk("i2c-handover: controller did not go idle -- NOT releasing the bus\n"); \
			return 0; \
		} \
		barrier_dsync_fence_full(); \
		*HANDOVER_FLAG(inst) = I2C_HANDOVER_MAGIC; \
		barrier_dsync_fence_full(); \
		printk("i2c-handover: bus released (flag 0x%08x @0x%08x)\n", \
		       (unsigned)I2C_HANDOVER_MAGIC, \
		       (unsigned)DT_INST_PROP(inst, flag_address)); \
		return 0; \
	} \
	SYS_INIT(i2c_handover_release_##inst, APPLICATION, I2C_HANDOVER_RELEASE_PRIO);

/* ---- acquire ---------------------------------------------------------- */
#define HANDOVER_ACQUIRE(inst) \
	static int i2c_handover_acquire_##inst(void) \
	{ \
		int64_t t0 = k_uptime_get(); \
\
		printk("i2c-handover: waiting for the other core to release the bus\n"); \
		while (*HANDOVER_FLAG(inst) != I2C_HANDOVER_MAGIC) { \
			if (k_uptime_get() - t0 >= I2C_HANDOVER_REPORT_MS) { \
				printk("i2c-handover: bus NOT released after 20 s -- not touching it, " \
				       "still waiting\n"); \
				t0 = k_uptime_get(); \
			} \
			k_msleep(I2C_HANDOVER_WAIT_STEP_MS); \
		} \
		*HANDOVER_FLAG(inst) = 0u; \
		barrier_dsync_fence_full(); \
		printk("i2c-handover: bus acquired\n"); \
		return 0; \
	} \
	SYS_INIT(i2c_handover_acquire_##inst, POST_KERNEL, 0);

#define HANDOVER_INST(inst) \
	COND_CODE_1(DT_INST_ENUM_HAS_VALUE(inst, role, release), \
	            (HANDOVER_RELEASE(inst)), \
	            (HANDOVER_ACQUIRE(inst)))

DT_INST_FOREACH_STATUS_OKAY(HANDOVER_INST)
