/* src/platform/bus2_he.c -- the HE's side of the I2C2 + GPIO5 lease (TR_HP_SOUND builds). The
 * protocol is src/ipc/tr_bus2.c (host-tested); this file is the Zephyr glue: the boot claim, the
 * per-frame tick, and stopping / re-arming this core's I2C2.
 *
 * Why app code and not the SDK's alp,i2c-handover (zephyr/soc-bridge/alif/i2c_handover.c): that
 * glue is one-way and its acquire side blocks at POST_KERNEL 0 until the release arrives -- right
 * for the camera bus (the HP's vision cannot start without it), wrong here: the HP's VISION must
 * never wait for the sound handshake, and the HE needs the bus BACK after the amp bring-up. */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>

#include "bus2_he.h"
#include "ipc/tr_bus2.h"

#define BUS2_I2C2 DT_NODELABEL(i2c2)
BUILD_ASSERT(DT_NODE_HAS_STATUS(BUS2_I2C2, okay),
             "TR_HP_SOUND: the HE's I2C2 carries the INA236 / BMI323 and must stay enabled");
BUILD_ASSERT(DT_REG_ADDR(BUS2_I2C2) == 0x49012000, "carrier bus 0 is SoC I2C2 0x49012000");
BUILD_ASSERT(DT_PROP(BUS2_I2C2, clock_frequency) == 100000,
             "the HE's I2C2 is standard mode, as the HP's (src/ipc/tr_bus2.h)");
/* The HP drives GPIO5 (SD_N P5_2, IRQZ P5_0) with read-modify-write of DR/DDR and its lpgpio /
 * GPIO5 driver init masks those ports' interrupts: this image must use neither. The RK055 shield
 * puts its backlight enable on GPIO5 (P5_5) and enables the port; the Riverdi RVT121 shield
 * (backlight PWM on P10_7) does not. */
BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_NODELABEL(gpio5), okay),
             "TR_HP_SOUND: GPIO5 is the HP's (amp SD_N / IRQZ); use a shield that does not use it "
             "(e1m_evk_rvt121hvdfwca0, not the RK055's P5_5 backlight)");

#define DW_IC_ENABLE        0x6Cu /* DesignWare APB I2C */
#define DW_IC_ENABLE_STATUS 0x9Cu

static tr_bus2_he_t              g_he;
static volatile tr_bus2_t *const g_b2 = (volatile tr_bus2_t *)TR_MEM_BUS2;

static void b2_barrier(void)
{
	barrier_dsync_fence_full();
}

/* Stop this core's I2C2: its IRQ line off first (the HP's transfers must not run our ISR), then
 * the controller; bounded wait for idle (a stuck controller is still handed over -- the HP
 * reprograms it with i2c_configure). */
static void b2_give(void *ctx)
{
	ARG_UNUSED(ctx);
	irq_disable(DT_IRQN(BUS2_I2C2));
	sys_write32(0u, DT_REG_ADDR(BUS2_I2C2) + DW_IC_ENABLE);
	for (int i = 0; i < 100 && (sys_read32(DT_REG_ADDR(BUS2_I2C2) + DW_IC_ENABLE_STATUS) & 1u);
	     i++) {
		k_busy_wait(100);
	}
}

/* The HP is done and its IRQ line is off: reprogram the controller (the HP's i2c_dw left it in
 * its own state) and turn this core's line back on. */
static void b2_take(void *ctx)
{
	ARG_UNUSED(ctx);
	(void)i2c_configure(DEVICE_DT_GET(BUS2_I2C2),
	                    I2C_SPEED_SET(I2C_SPEED_STANDARD) | I2C_MODE_CONTROLLER);
	irq_enable(DT_IRQN(BUS2_I2C2));
}

static int64_t b2_now_ms(void *ctx)
{
	ARG_UNUSED(ctx);
	return k_uptime_get();
}

static void b2_sleep_ms(void *ctx, uint32_t ms)
{
	ARG_UNUSED(ctx);
	k_msleep(ms);
}

static const tr_bus2_ops_t g_ops = {
	.barrier  = b2_barrier,
	.give     = b2_give,
	.take     = b2_take,
	.now_ms   = b2_now_ms,
	.sleep_ms = b2_sleep_ms,
};

/* POST_KERNEL 0: the kernel clock is up (the bounded wait needs it), and still ahead of every
 * driver (the i2c_dw instance is priority 40+) and of the display shield's expander. The HP's
 * bring-up may already be running from an earlier session of this core: void the offer, then
 * wait (bounded) while the HP is inside a step that uses the bus. */
static int bus2_he_boot(void)
{
	g_b2->he_regains = 0u;
	if (tr_bus2_he_boot(&g_he, g_b2, &g_ops, TR_BUS2_HE_BOOT_WAIT_MS)) {
		printk("RESULT FAIL: the HP was still in an I2C2 bring-up step after %u ms -- the HE "
		       "proceeds; that bring-up aborts at its next step\n",
		       (unsigned)TR_BUS2_HE_BOOT_WAIT_MS);
	}
	return 0;
}
SYS_INIT(bus2_he_boot, POST_KERNEL, 0);

void tr_bus2_he_frame(void)
{
	uint8_t before = g_he.st;

	tr_bus2_he_tick(&g_he, g_b2, &g_ops, k_cycle_get_32() ^ (uint32_t)k_uptime_get());
	if (g_he.st != before) {
		printk("bus2    : I2C2 %s\n",
		       g_he.st == TR_BUS2_ST_OFFERED   ? "offered to the HP"
		       : g_he.st == TR_BUS2_ST_CLAIMED ? "leased by the HP"
		                                       : "back on the HE");
	}
}

bool tr_bus2_he_owns(void)
{
	return tr_bus2_he_owns_bus(&g_he);
}
