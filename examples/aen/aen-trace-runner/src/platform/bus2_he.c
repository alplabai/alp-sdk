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
#include "tr_i2c2_rearm.h"

#define BUS2_I2C2 TR_I2C2_NODE
BUILD_ASSERT(DT_NODE_HAS_STATUS(BUS2_I2C2, okay),
             "TR_HP_SOUND: the HE's I2C2 carries the INA236 / BMI323 and must stay enabled");
BUILD_ASSERT(DT_PROP(BUS2_I2C2, clock_frequency) == 100000,
             "the HE's I2C2 is standard mode, as the HP's (src/ipc/tr_bus2.h)");
/* The HP drives GPIO5 (SD_N P5_2, IRQZ P5_0) with read-modify-write of DR/DDR and its lpgpio /
 * GPIO5 driver init masks those ports' interrupts: this image must not instantiate the GPIO5
 * driver. (It does touch GPIO5 DR/DDR bits 6/7 by raw MMIO for the SCL bus-clear, tr_i2c2_rearm.h,
 * only while it owns the bus.) The RK055 shield
 * puts its backlight enable on GPIO5 (P5_5) and enables the port; the Riverdi RVT121 shield
 * (backlight PWM on P10_7) does not. */
BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_NODELABEL(gpio5), okay),
             "TR_HP_SOUND: GPIO5 is the HP's (amp SD_N / IRQZ); use a shield that does not use it "
             "(e1m_evk_rvt121hvdfwca0, not the RK055's P5_5 backlight)");
/* The same goes for the LP-GPIO port (0x42002000): the HP drives the CC3501E's WIFI_EN / nRESET
 * (P15_5 / P15_1) on it, and a gpio_dw instance on this core masks that port's interrupts. The
 * board's RV3028 RTC is the only user of this core's lpgpio (its INT line): an image that builds
 * its driver arms the port, so a TR_HP_SOUND build must not. */
BUILD_ASSERT(
    !IS_ENABLED(CONFIG_RTC_RV3028),
    "TR_HP_SOUND: the HE must not build the RV3028 RTC driver (it arms the lpgpio port the "
    "HP drives for the CC3501E's WIFI_EN / nRESET)");

static tr_bus2_he_t g_he;
static bool         g_armed; /* tr_bus2_he_arm(): this core's own I2C2 users are open */
static volatile tr_bus2_t *const g_b2 = (volatile tr_bus2_t *)TR_MEM_BUS2;

static void b2_barrier(void)
{
	barrier_dsync_fence_full();
}

/* Stop this core's I2C2: its IRQ line off first (the HP's transfers must not run our ISR), then
 * the controller; bounded wait for idle (a stuck controller is still handed over -- the HP
 * re-arms it, tr_i2c2_quiesce()). */
static void b2_give(void *ctx)
{
	ARG_UNUSED(ctx);
	irq_disable(TR_I2C2_IRQN);
	tr_i2c2_stop();
}

/* The HP is done and its IRQ line is off (or it died): the other core's last state is still in the
 * controller, the NVIC and the driver. Quiesce (IC_ENABLE = 0 and wait, IC_INTR_MASK = 0, clear the
 * pending IRQ, reset the driver's sync semaphore, bus-recover when busy or SDA low), THEN
 * reprogram it and turn this core's line back on. */
static void b2_take(void *ctx)
{
	ARG_UNUSED(ctx);
	if (!tr_i2c2_quiesce(DEVICE_DT_GET(BUS2_I2C2), true)) {
		printk("bus2    : WARN SDA still low after the bus-clear\n");
	}
	(void)i2c_configure(DEVICE_DT_GET(BUS2_I2C2),
	                    I2C_SPEED_SET(I2C_SPEED_STANDARD) | I2C_MODE_CONTROLLER);
	irq_enable(TR_I2C2_IRQN);
}

/* The lease holder stopped beating: its core is dead, perhaps mid-transfer. Stop the controller
 * and clock the bus clear (all 9 clocks + STOP: the slave may be mid-byte); b2_take() follows. */
static void b2_reclaim(void *ctx)
{
	ARG_UNUSED(ctx);
	irq_disable(TR_I2C2_IRQN);
	tr_i2c2_stop();
	bool sda = tr_i2c2_bus_clear(true);

	printk(
	    "bus2    : HP silent for %u ms while holding I2C2: reclaimed, IC_ENABLE=0, SCL bus-clear "
	    "-> SDA %s\n",
	    (unsigned)TR_BUS2_HP_DEAD_MS,
	    sda ? "high" : "STILL LOW");
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
	.reclaim  = b2_reclaim,
	.now_ms   = b2_now_ms,
	.sleep_ms = b2_sleep_ms,
};

/* POST_KERNEL 0: the kernel clock is up (the bounded wait needs it), and still ahead of every
 * driver (the i2c_dw instance is priority 40+) and of the display shield's expander. The HP's
 * bring-up may already be running from an earlier session of this core: void the offer, then
 * wait (bounded) while the HP is inside a step that uses the bus. */
static int bus2_he_boot(void)
{
	g_b2->he_regains  = 0u;
	g_b2->he_reclaims = 0u;
	if (tr_bus2_he_boot(&g_he, g_b2, &g_ops, TR_BUS2_HE_BOOT_WAIT_MS)) {
		printk("RESULT FAIL: the HP was still in an I2C2 bring-up step after %u ms -- the HE "
		       "proceeds; that bring-up aborts at its next step\n",
		       (unsigned)TR_BUS2_HE_BOOT_WAIT_MS);
	} else if (g_he.boot_stale) {
		/* A warm reset keeps SRAM0: a dead HP's BUS is still there and nothing moves. */
		printk("WARN: hp_state=BUS in the lease record but hp_beat / hp_state did not move for %u "
		       "ms -- a dead HP's leftover from a previous session, ignored\n",
		       (unsigned)TR_BUS2_HE_BOOT_WAIT_MS);
	}
	return 0;
}
SYS_INIT(bus2_he_boot, POST_KERNEL, 0);

void tr_bus2_he_arm(void)
{
	g_armed = true;
}

void tr_bus2_he_frame(void)
{
	static bool dc_told;
	uint8_t     before   = g_he.st;
	uint32_t    reclaims = g_b2->he_reclaims;

	if (!g_armed) {
		return; /* the BMI323 / INA236 opens have not run yet: no offer before they did */
	}

	/* The lease record lives in SRAM0, uncached by the MPU region over its page (the board
	 * overlay) and by CONFIG_DCACHE=n. A warm RAM-run can still inherit CCR.DC=1 from a previous
	 * image (CONFIG_DCACHE=n does not switch the cache off): then neither core can trust the
	 * record, and this core neither offers nor claims. */
	if ((SCB->CCR & SCB_CCR_DC_Msk) != 0u) {
		if (!dc_told) {
			printk("bus2    : D-cache is ON (SCB->CCR.DC): the lease is not offered\n");
			dc_told = true;
		}
		return;
	}
	tr_bus2_he_tick(&g_he, g_b2, &g_ops, k_cycle_get_32() ^ (uint32_t)k_uptime_get());
	if (g_b2->he_reclaims != reclaims) {
		printk("bus2    : lease reclaimed from a silent HP (he_reclaims=%u)\n",
		       (unsigned)g_b2->he_reclaims);
	}
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
