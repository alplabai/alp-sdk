/*
 * src/platform/i2c1_handoff.c -- the HE side of the I2C1 handoff
 * (ipc/tr_i2c1_flag.h).
 *
 * The RVT121 shield enables &i2c1 on the HE so the SN65DSI83 driver can
 * configure the bridge at boot (once; it needs the DSI clock lane running,
 * which its own init sequences). With TR_INPUT_NPU the HP owns that same
 * controller (0x49011000, one DesignWare block both cores can address) for
 * the camera, so after the bridge is up the HE
 *   1. masks its NVIC line for the controller -- else this core's i2c_dw ISR
 *      would also run on the HP's transfers and clear INTR_MASK / status
 *      under it (the same hazard rail5v_power.c documents for I2C2);
 *   2. clears IC_ENABLE and waits for IC_ENABLE_STATUS to read idle, which
 *      releases SCL/SDA (the pads keep their I2C1 function, the HP's own
 *      driver init takes over from there);
 *   3. only then writes the "free" word the HP waits for.
 * The touch controller is disabled in the devicetree (panel_rvt121_window.overlay),
 * so nothing on the HE needs I2C1 again.
 *
 * The word is cleared at PRE_KERNEL_1 priority 0 on every HE boot: SRAM0 is
 * always-on, so a warm reset leaves the previous boot's magic behind.
 */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>

#include "../ipc/tr_i2c1_flag.h"
#include "../ipc/tr_memmap.h"
#include "i2c1_handoff.h"

#define I2C1_NODE           DT_NODELABEL(i2c1)
#define DW_IC_ENABLE        0x6Cu /* DesignWare APB I2C */
#define DW_IC_ENABLE_STATUS 0x9Cu
#define DW_IC_EN_BIT        BIT(0)

BUILD_ASSERT(DT_REG_ADDR(I2C1_NODE) == 0x49011000u, "I2C1 is the camera SCCB bus (0x49011000)");

static int tr_i2c1_free_clear_init(void)
{
	*(volatile uint32_t *)TR_MEM_I2C1_FREE = 0u;
	return 0;
}
SYS_INIT(tr_i2c1_free_clear_init, PRE_KERNEL_1, 0);

bool tr_i2c1_release(void)
{
	uintptr_t base = DT_REG_ADDR(I2C1_NODE);

	irq_disable(DT_IRQN(I2C1_NODE));
	sys_write32(0u, base + DW_IC_ENABLE);
	for (int i = 0; i < 100 && (sys_read32(base + DW_IC_ENABLE_STATUS) & DW_IC_EN_BIT); i++) {
		k_busy_wait(100);
	}
	if (sys_read32(base + DW_IC_ENABLE_STATUS) & DW_IC_EN_BIT) {
		printk("i2c1    : controller did not go idle -- NOT handing I2C1 to the HP\n");
		return false;
	}
	barrier_dsync_fence_full();
	*(volatile uint32_t *)TR_MEM_I2C1_FREE = TR_I2C1_FREE_MAGIC;
	barrier_dsync_fence_full();
	printk("i2c1    : released to the HP (flag 0x%08x @0x%08x)\n",
	       (unsigned)TR_I2C1_FREE_MAGIC,
	       (unsigned)TR_MEM_I2C1_FREE);
	return true;
}
