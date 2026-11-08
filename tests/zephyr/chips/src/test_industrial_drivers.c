/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Behaviour regressions for the a4988 / drv8825 / drv8833 / max31855 /
 * max31865 drivers.  The drivers' alp_gpio_write / alp_pwm_set_duty /
 * alp_spi_transceive calls are intercepted with the linker (--wrap in this
 * test's CMakeLists.txt) so a test sees the exact ORDER and values of the
 * writes -- the PWM backend double has no observable state.  Outside a test
 * capture window every wrapped call forwards to the real function, so the
 * other tests in this suite are unaffected.
 */

#include <string.h>
#include <zephyr/ztest.h>

#include "alp/chips/a4988.h"
#include "alp/chips/drv8825.h"
#include "alp/chips/drv8833.h"
#include "alp/chips/max31855.h"
#include "alp/chips/max31865.h"
#include "alp/peripheral.h"
#include "alp/pwm.h"

alp_status_t __real_alp_gpio_write(alp_gpio_t *pin, bool level);
alp_status_t __real_alp_pwm_set_duty(alp_pwm_t *pwm, uint32_t pulse_ns);
alp_status_t __real_alp_spi_transceive(alp_spi_t *bus, const uint8_t *tx, uint8_t *rx, size_t len);

enum ev_kind { EV_GPIO, EV_PWM };

struct ev {
	enum ev_kind kind;
	const void  *handle;
	uint32_t     val;
};

static bool      capturing;
static struct ev evs[16];
static size_t    n_evs;
static uint8_t   spi_miso[4];
static size_t    spi_calls;
static size_t    spi_last_len;
static uint8_t   spi_last_tx[4];

alp_status_t __wrap_alp_gpio_write(alp_gpio_t *pin, bool level)
{
	if (!capturing) return __real_alp_gpio_write(pin, level);
	if (n_evs < ARRAY_SIZE(evs)) evs[n_evs++] = (struct ev){ EV_GPIO, pin, level };
	return ALP_OK;
}

alp_status_t __wrap_alp_pwm_set_duty(alp_pwm_t *pwm, uint32_t pulse_ns)
{
	if (!capturing) return __real_alp_pwm_set_duty(pwm, pulse_ns);
	if (n_evs < ARRAY_SIZE(evs)) evs[n_evs++] = (struct ev){ EV_PWM, pwm, pulse_ns };
	return ALP_OK;
}

alp_status_t __wrap_alp_spi_transceive(alp_spi_t *bus, const uint8_t *tx, uint8_t *rx, size_t len)
{
	if (!capturing) return __real_alp_spi_transceive(bus, tx, rx, len);
	spi_calls++;
	spi_last_len = len;
	if (tx != NULL) memcpy(spi_last_tx, tx, len < sizeof(spi_last_tx) ? len : sizeof(spi_last_tx));
	if (rx != NULL) memcpy(rx, spi_miso, len < sizeof(spi_miso) ? len : sizeof(spi_miso));
	return ALP_OK;
}

static void begin(void)
{
	capturing = true;
	n_evs     = 0;
	spi_calls = 0;
	memset(spi_miso, 0, sizeof(spi_miso));
}

/* Distinct non-NULL fake handles; the wrappers never dereference them. */
static char h[8];
#define PWM(i)  ((alp_pwm_t *)&h[i])
#define GPIO(i) ((alp_gpio_t *)&h[i])
#define SPI()   ((alp_spi_t *)&h[7])

static const struct ev *find_gpio(const void *handle)
{
	for (size_t i = 0; i < n_evs; i++)
		if (evs[i].kind == EV_GPIO && evs[i].handle == handle) return &evs[i];
	return NULL;
}

ZTEST(alp_chips, test_a4988_init_disables_outputs)
{
	a4988_t dev;
	begin();
	zassert_equal(a4988_init(&dev, PWM(0), GPIO(1), GPIO(2), NULL, NULL, NULL), ALP_OK);
	const struct ev *e = find_gpio(GPIO(2));
	capturing          = false;
	zassert_not_null(e, "nENABLE never driven");
	zassert_equal(e->val, 1u, "nENABLE must be high (coils off)");
}

ZTEST(alp_chips, test_a4988_set_enable_is_active_low)
{
	a4988_t dev;
	begin();
	zassert_equal(a4988_init(&dev, PWM(0), GPIO(1), GPIO(2), NULL, NULL, NULL), ALP_OK);
	n_evs = 0;
	zassert_equal(a4988_set_enable(&dev, true), ALP_OK);
	zassert_equal(evs[0].val, 0u);
	zassert_equal(a4988_set_enable(&dev, false), ALP_OK);
	zassert_equal(evs[1].val, 1u);
	capturing = false;
}

ZTEST(alp_chips, test_drv8825_init_disables_outputs)
{
	drv8825_t dev;
	begin();
	zassert_equal(drv8825_init(&dev, PWM(0), GPIO(1), GPIO(2), NULL, NULL, NULL), ALP_OK);
	const struct ev *e = find_gpio(GPIO(2));
	capturing          = false;
	zassert_not_null(e, "nENBL never driven");
	zassert_equal(e->val, 1u, "nENBL must be high (coils off)");
}

ZTEST(alp_chips, test_drv8825_set_enable_is_active_low)
{
	drv8825_t dev;
	begin();
	zassert_equal(drv8825_init(&dev, PWM(0), GPIO(1), GPIO(2), NULL, NULL, NULL), ALP_OK);
	n_evs = 0;
	zassert_equal(drv8825_set_enable(&dev, true), ALP_OK);
	zassert_equal(evs[0].val, 0u);
	zassert_equal(drv8825_set_enable(&dev, false), ALP_OK);
	zassert_equal(evs[1].val, 1u);
	/* Without nENBL there is nothing to switch. */
	zassert_equal(drv8825_init(&dev, PWM(0), GPIO(1), NULL, NULL, NULL, NULL), ALP_OK);
	capturing = false;
	zassert_equal(drv8825_set_enable(&dev, true), ALP_ERR_NOSUPPORT);
	zassert_equal(drv8825_set_enable(NULL, true), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_drv8833_init_zeroes_pwm_before_wake)
{
	drv8833_t dev;
	begin();
	zassert_equal(drv8833_init(&dev, PWM(0), PWM(1), PWM(2), PWM(3), GPIO(4)), ALP_OK);
	capturing = false;
	zassert_equal(n_evs, 5u);
	for (size_t i = 0; i < 4; i++) {
		zassert_equal(evs[i].kind, EV_PWM, "duty writes must precede nSLEEP");
		zassert_equal(evs[i].val, 0u);
	}
	zassert_equal(evs[4].kind, EV_GPIO);
	zassert_equal(evs[4].handle, GPIO(4));
	zassert_equal(evs[4].val, 1u, "nSLEEP high = awake");
}

ZTEST(alp_chips, test_max31855_fault_returns_io_with_outputs)
{
	max31855_t dev;
	int32_t    tc = 0, cj = 0;
	uint8_t    flags = 0;
	zassert_equal(max31855_init(&dev, SPI()), ALP_OK);
	begin();
	/* TC = 25.00 C (100 LSB), D16 fault set, OC bit set. */
	uint32_t word  = (100u << 18) | 0x10000u | 0x1u;
	spi_miso[0]    = (uint8_t)(word >> 24);
	spi_miso[1]    = (uint8_t)(word >> 16);
	spi_miso[2]    = (uint8_t)(word >> 8);
	spi_miso[3]    = (uint8_t)word;
	alp_status_t s = max31855_read(&dev, &tc, &cj, &flags);
	capturing      = false;
	zassert_equal(s, ALP_ERR_IO);
	zassert_equal(tc, 25000);
	zassert_equal(flags, 0x11u);
}

ZTEST(alp_chips, test_max31855_ok_without_fault)
{
	max31855_t dev;
	int32_t    tc = 0;
	zassert_equal(max31855_init(&dev, SPI()), ALP_OK);
	begin();
	spi_miso[0]    = 0x01;
	spi_miso[1]    = 0x90;
	alp_status_t s = max31855_read(&dev, &tc, NULL, NULL);
	capturing      = false;
	zassert_equal(s, ALP_OK);
	zassert_equal(tc, 25000);
}

ZTEST(alp_chips, test_max31865_read_rtd_single_transceive)
{
	max31865_t dev;
	uint16_t   rtd   = 0;
	bool       fault = true;
	zassert_equal(max31865_init(&dev, SPI()), ALP_OK);
	begin();
	/* rx[0] is the address phase; MSB 0x40, LSB 0x02 -> (0x40 << 7) | 1. */
	spi_miso[1]    = 0x40;
	spi_miso[2]    = 0x02;
	alp_status_t s = max31865_read_rtd(&dev, &rtd, &fault);
	capturing      = false;
	zassert_equal(s, ALP_OK);
	zassert_equal(spi_calls, 1u, "MSB and LSB must come from one CS window");
	zassert_equal(spi_last_len, 3u);
	zassert_equal(spi_last_tx[0], MAX31865_REG_RTD_MSB);
	zassert_equal(rtd, (uint16_t)((0x40u << 7) | 1u));
	zassert_false(fault);
}
