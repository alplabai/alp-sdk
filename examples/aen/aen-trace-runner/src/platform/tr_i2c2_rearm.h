/* src/platform/tr_i2c2_rearm.h -- put this core's DesignWare I2C2 (0x49012000, carrier bus 0) into
 * a known state before it is armed on a bus the OTHER core has just used or abandoned. Both cores of
 * the TR_HP_SOUND image share the one controller and its IRQ 134 (src/ipc/tr_bus2.h), so whoever
 * takes the bus over finds it in the other core's last state: enabled, a transfer half done, INTR_MASK
 * and the NVIC pending bit left by the other core's ISR, the driver's sync semaphore given by a
 * transfer it did not make, a slave stretching SCL or holding SDA low. Used by the HE (bus2_he.c
 * b_take / b_reclaim) and by the HP's lease acquisition (sound/src/main.c), Zephyr-only, header-only.
 *
 * The DesignWare driver registers no bus-recovery callback on this SoC (i2c_recover_bus() would be a
 * no-op), so the SCL bus-clear is done here: both pads to GPIO function, SCL clocked 9 times as an
 * open-drain line (GPIO5 DDR bit = drive low, cleared = released to the carrier's pull-up) until SDA
 * is released, then a STOP, then the pads back to the I2C2 function from the DT pinctrl state.
 * P5_6 = SCL, P5_7 = SDA (pinctrl_i2c2: PIN_P5_6__I2C2_SCL_C, PIN_P5_7__I2C2_SDA_C); GPIO5 is
 * 0x49005000 with the DW GPIO register map (SWPORTA_DR 0x00, SWPORTA_DDR 0x04, EXT_PORTA 0x50,
 * gpio_dw_registers.h). Alif measured that the GPIO functional clock gate is not needed for pad
 * drive (zephyr/drivers/gpio/gpio_clk_alif.c); only bits 6 and 7 of DR / DDR are touched. */
#ifndef TR_PLATFORM_I2C2_REARM_H
#define TR_PLATFORM_I2C2_REARM_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#include <cmsis_core.h>

#include "i2c_dw.h" /* struct i2c_dw_dev_config: the sync semaphore (drivers/i2c on the include path) */

#define TR_I2C2_NODE DT_NODELABEL(i2c2)
#define TR_I2C2_BASE DT_REG_ADDR(TR_I2C2_NODE)
#define TR_I2C2_IRQN DT_IRQN(TR_I2C2_NODE)

BUILD_ASSERT(TR_I2C2_BASE == 0x49012000, "carrier bus 0 is SoC I2C2 0x49012000");
BUILD_ASSERT(TR_I2C2_IRQN == 134, "SoC I2C2 is IRQ 134 (in both M55 NVICs)");

#define TR_I2C2_IC_INTR_MASK     0x30u
#define TR_I2C2_IC_ENABLE        0x6Cu
#define TR_I2C2_IC_STATUS        0x70u
#define TR_I2C2_IC_ENABLE_STATUS 0x9Cu
#define TR_I2C2_STATUS_BUSY      0x21u /* IC_STATUS.ACTIVITY (bit 0) | MST_ACTIVITY (bit 5) */

#define TR_GPIO5_BASE 0x49005000u
#define TR_GPIO_DR    0x00u
#define TR_GPIO_DDR   0x04u
#define TR_GPIO_EXT   0x50u
#define TR_I2C2_SCL   (1u << 6) /* P5_6 */
#define TR_I2C2_SDA   (1u << 7) /* P5_7 */
#define TR_PAD_REN    (1u << 16)

/* The i2c2 node's own pin state, to put the pads back after the bus-clear. (Not
 * PINCTRL_DT_STATE_PINS_DEFINE: that exists only with CONFIG_PINCTRL_DYNAMIC. The Alif SoC's
 * Z_PINCTRL_STATE_PINS_INIT already ends in the closing "};".) */
static const pinctrl_soc_pin_t                tr_i2c2_pads[]      = Z_PINCTRL_STATE_PINS_INIT(
    TR_I2C2_NODE,
    pinctrl_0) static const pinctrl_soc_pin_t tr_i2c2_gpio_pads[] = { PIN_P5_6__GPIO | TR_PAD_REN,
	                                                                  PIN_P5_7__GPIO | TR_PAD_REN };

/* IC_ENABLE = 0 and wait (bounded, 10 ms) for IC_ENABLE_STATUS to clear. */
static inline void tr_i2c2_stop(void)
{
	sys_write32(0u, TR_I2C2_BASE + TR_I2C2_IC_ENABLE);
	for (int i = 0; i < 100 && (sys_read32(TR_I2C2_BASE + TR_I2C2_IC_ENABLE_STATUS) & 1u) != 0u;
	     i++) {
		k_busy_wait(100);
	}
}

static inline void tr_gpio5_ddr(uint32_t set, uint32_t clr)
{
	uint32_t v = sys_read32(TR_GPIO5_BASE + TR_GPIO_DDR);

	sys_write32((v | set) & ~clr, TR_GPIO5_BASE + TR_GPIO_DDR);
}

/* True when SDA reads high on the pad (pads must already be in GPIO function). */
static inline bool tr_i2c2_sda_high(void)
{
	return (sys_read32(TR_GPIO5_BASE + TR_GPIO_EXT) & TR_I2C2_SDA) != 0u;
}

/* The controller must be stopped (tr_i2c2_stop()). Clocks SCL up to 9 times when SDA reads low or
 * `busy` (IC_STATUS showed activity, or a core died mid-transfer: the slave may be mid-byte with SDA
 * released at this very instant, so then all 9 clocks run); not at all on an idle bus. Returns true
 * when SDA is high afterwards. */
static inline bool tr_i2c2_bus_clear(bool busy)
{
	bool ok;

	/* open-drain emulation: the output value stays 0, DDR bit set = driven low, clear = released.
	 * Both are set up BEFORE the pads leave the I2C function, so the switch to GPIO never drives
	 * a line. */
	sys_write32(sys_read32(TR_GPIO5_BASE + TR_GPIO_DR) & ~(TR_I2C2_SCL | TR_I2C2_SDA),
	            TR_GPIO5_BASE + TR_GPIO_DR);
	tr_gpio5_ddr(0u, TR_I2C2_SCL | TR_I2C2_SDA);
	(void)pinctrl_configure_pins(tr_i2c2_gpio_pads, ARRAY_SIZE(tr_i2c2_gpio_pads), 0U);
	k_busy_wait(10);
	if (busy || !tr_i2c2_sda_high()) {
		for (int i = 0; i < 9; i++) { /* 9 clocks at ~100 kHz */
			tr_gpio5_ddr(TR_I2C2_SCL, 0u);
			k_busy_wait(5);
			tr_gpio5_ddr(0u, TR_I2C2_SCL);
			k_busy_wait(5);
			if (!busy && tr_i2c2_sda_high()) {
				break;
			}
		}
		tr_gpio5_ddr(TR_I2C2_SDA, 0u); /* STOP: SDA low while SCL is high ... */
		k_busy_wait(5);
		tr_gpio5_ddr(0u, TR_I2C2_SDA); /* ... then SDA released */
		k_busy_wait(5);
	}
	ok = tr_i2c2_sda_high();
	(void)pinctrl_configure_pins(tr_i2c2_pads, ARRAY_SIZE(tr_i2c2_pads), 0U);
	return ok;
}

/* Re-arm preparation, in this order, before i2c_configure() and irq_enable():
 *  1 IC_ENABLE = 0 and wait for IC_ENABLE_STATUS to clear;
 *  2 IC_INTR_MASK = 0;
 *  3 clear the NVIC pending bit of IRQ 134;
 *  4 reset the driver's sync semaphore (only once the driver has been initialised: driver_up);
 *  5 bus-recover when IC_STATUS shows activity or SDA reads low.
 * Returns false when SDA is still low after the bus-clear. */
static inline bool tr_i2c2_quiesce(const struct device *dev, bool driver_up)
{
	tr_i2c2_stop();
	sys_write32(0u, TR_I2C2_BASE + TR_I2C2_IC_INTR_MASK);
	NVIC_ClearPendingIRQ(TR_I2C2_IRQN);
	if (driver_up) {
		struct i2c_dw_dev_config *dw = dev->data;

		k_sem_reset(&dw->device_sync_sem);
	}
	return tr_i2c2_bus_clear((sys_read32(TR_I2C2_BASE + TR_I2C2_IC_STATUS) & TR_I2C2_STATUS_BUSY) !=
	                         0u);
}

#endif /* TR_PLATFORM_I2C2_REARM_H */
