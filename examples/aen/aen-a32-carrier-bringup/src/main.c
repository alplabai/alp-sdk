/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * aen-a32-carrier-bringup -- exercise the E1M-EVK carrier peripherals
 * from a Linux/Yocto user-space app on the E1M-AEN801 Cortex-A32.
 *
 * Demonstrates the portable alp_* surface mapping onto the Linux
 * userspace ABIs:
 *   - alp_i2c_* over /dev/i2c-N (i2c-dev): bus scan + chip drivers
 *   - tcal9538 IO-expander @0x72 toggle/read
 *   - bmi323 (0x68) / icm42670 (0x69) IMU chip-id, runtime-detected
 *   - alp_gpio_* over the gpiochip v2 chardev ABI
 *
 * Bring-up / demo utility, not a production path.  Addresses come
 * from <alp/boards/alp_e1m_evk.h>; nothing is invented.
 *
 * Board-gated values (TODO(e1m-evk-hw)) are isolated as named
 * constants -- the code compiles + links now; fill them in on the
 * bench from the live /dev enumeration.  See README.md.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "alp/peripheral.h"
#include "alp/chips/tcal9538.h"
#include "alp/chips/bmi323.h"
#include "alp/chips/icm42670.h"
#include "alp/boards/alp_e1m_evk.h"

/*
 * Board-gated runtime mappings -- TODO(e1m-evk-hw).
 *
 * The AEN801 carrier DTS routes EVK_I2C_BUS_SENSORS (E1M_I2C0) to Alif
 * I2C2, but the kernel assigns the /dev/i2c-N adapter index at boot.
 * Confirm with `i2cdetect -l` on the target and set N to match.
 */
#define AEN_SENSOR_I2C_ADAPTER 2u /* TODO(e1m-evk-hw): confirm /dev/i2c-N */

/*
 * alp_gpio_* packs pin_id = (gpiochip << 16) | line_offset.  Resolve
 * the gpiochip index + line offset for the LED + IMU-INT pads from the
 * live `gpiodetect` / `gpioinfo` enumeration on the booted board.
 */
#define AEN_GPIO_PACK(chip, line) (((uint32_t)(chip) << 16) | (uint32_t)(line))
#define AEN_PIN_LED_GREEN         AEN_GPIO_PACK(0, 0) /* TODO(e1m-evk-hw): EVK_PIN_LED_GREEN */
#define AEN_PIN_BMI323_INT        AEN_GPIO_PACK(0, 0) /* TODO(e1m-evk-hw): EVK_PIN_BMI323_INT1 */

static int g_pass;
static int g_fail;

#define STEP_OK(msg)                                                                               \
	do {                                                                                           \
		printf("[ OK ] %s\n", (msg));                                                              \
		g_pass++;                                                                                  \
	} while (0)
#define STEP_FAIL(msg)                                                                             \
	do {                                                                                           \
		printf("[FAIL] %s\n", (msg));                                                              \
		g_fail++;                                                                                  \
	} while (0)

static void bus_scan(alp_i2c_t *bus)
{
	printf("-- i2c scan on adapter %u --\n", AEN_SENSOR_I2C_ADAPTER);
	for (uint8_t a = 0x08u; a <= 0x77u; a++) {
		uint8_t dummy = 0;
		if (alp_i2c_read(bus, a, &dummy, 1) == ALP_OK) printf("   device @ 0x%02x\n", a);
	}
}

static void probe_expander(alp_i2c_t *bus)
{
	tcal9538_t exp;
	if (tcal9538_init(&exp, bus, EVK_I2C_ADDR_TCAL9538) != ALP_OK) {
		STEP_FAIL("tcal9538 init @0x72");
		return;
	}
	uint8_t port = 0;
	if (tcal9538_set_direction(&exp, 0, TCAL9538_DIR_OUTPUT) == ALP_OK &&
	    tcal9538_set(&exp, 0, true) == ALP_OK && tcal9538_set(&exp, 0, false) == ALP_OK &&
	    tcal9538_read_all(&exp, &port) == ALP_OK) {
		printf("   tcal9538 port = 0x%02x\n", port);
		STEP_OK("tcal9538 toggle + read");
	} else {
		STEP_FAIL("tcal9538 toggle + read");
	}
	tcal9538_deinit(&exp);
}

static void probe_imu(alp_i2c_t *bus)
{
	bmi323_t bmi;
	if (bmi323_init(&bmi, bus, EVK_I2C_ADDR_BMI323) == ALP_OK) {
		uint8_t id = 0;
		bmi323_read_id(&bmi, &id);
		printf("   bmi323 chip-id = 0x%02x (expect 0x%02x)\n", id, BMI323_CHIP_ID);
		STEP_OK("primary IMU = bmi323 @0x68");
		bmi323_deinit(&bmi);
		return;
	}
	icm42670_t icm;
	if (icm42670_init(&icm, bus, EVK_I2C_ADDR_ICM42670) == ALP_OK) {
		uint8_t id = 0;
		icm42670_read_id(&icm, &id);
		printf("   icm42670 who_am_i = 0x%02x (expect 0x%02x)\n", id, ICM42670_WHO_AM_I_VAL);
		STEP_OK("alternate IMU = icm42670 @0x69");
		icm42670_deinit(&icm);
		return;
	}
	STEP_FAIL("IMU (neither bmi323@0x68 nor icm42670@0x69 answered)");
}

static void probe_gpio(void)
{
	alp_gpio_t *led = alp_gpio_open(AEN_PIN_LED_GREEN);
	if (led != NULL && alp_gpio_configure(led, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE) == ALP_OK &&
	    alp_gpio_write(led, true) == ALP_OK) {
		alp_gpio_write(led, false);
		STEP_OK("SoC GPIO LED drive (gpiochip)");
	} else {
		STEP_FAIL("SoC GPIO LED drive (set AEN_PIN_LED_GREEN)");
	}
	if (led != NULL) alp_gpio_close(led);

	alp_gpio_t *intp = alp_gpio_open(AEN_PIN_BMI323_INT);
	bool        lvl  = false;
	if (intp != NULL && alp_gpio_configure(intp, ALP_GPIO_INPUT, ALP_GPIO_PULL_NONE) == ALP_OK &&
	    alp_gpio_read(intp, &lvl) == ALP_OK) {
		printf("   BMI323 INT1 line = %d\n", (int)lvl);
		STEP_OK("SoC GPIO INT read (gpiochip)");
	} else {
		STEP_FAIL("SoC GPIO INT read (set AEN_PIN_BMI323_INT)");
	}
	if (intp != NULL) alp_gpio_close(intp);
}

int main(void)
{
	printf("== aen-a32-carrier-bringup (E1M-EVK on E1M-AEN801) ==\n");

	alp_i2c_config_t cfg = {
		.bus_id     = AEN_SENSOR_I2C_ADAPTER,
		.bitrate_hz = 400000u,
	};
	alp_i2c_t *bus = alp_i2c_open(&cfg);
	if (bus == NULL) {
		STEP_FAIL("alp_i2c_open(sensor bus)");
		return 1;
	}
	STEP_OK("alp_i2c_open(sensor bus)");

	bus_scan(bus);
	probe_expander(bus);
	probe_imu(bus);
	alp_i2c_close(bus);

	probe_gpio();

	printf("== done: %d ok, %d fail ==\n", g_pass, g_fail);
	return (g_fail != 0) ? 1 : 0;
}
