/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * CC3501E GPIO proxy -- delegated-path regression test (issue #1618).
 *
 * Only meaningful under CONFIG_ALP_SDK_GPIO_CC3501E_PROXY, which the
 * alp_sdk.peripheral.cc3501e_proxy scenario turns on; every other
 * scenario in this directory builds this file as an empty translation
 * unit.  Enabling the proxy globally would route EVERY existing GPIO
 * test through it, which is not what those tests are pinning.
 *
 * Deliberately NOT using the <alp/testing/gpio.h> virtual backend.  It
 * registers at priority 255 and REPLACES the platform backend -- which
 * is exactly the backend the proxy delegates into -- so it would
 * exercise the double's own callback plumbing and prove nothing about
 * the owner pointer the delegated path builds.  This runs against the
 * real gpio_emul-backed Zephyr backend with the proxy layered on top.
 */

#ifdef CONFIG_ALP_SDK_GPIO_CC3501E_PROXY

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/ztest.h>

#include "alp/peripheral.h"

static alp_gpio_t *g_seen_handle;
static void       *g_seen_user;
static int         g_fired;

static void proxy_cb(alp_gpio_t *pin, void *user)
{
	g_fired++;
	g_seen_handle = pin;
	g_seen_user   = user;
}

ZTEST(alp_peripheral, test_gpio_proxy_delegated_cb_gets_real_handle)
{
	/* Two handles, so the proxy's _sides[] slot immediately after the
	 * first one is claimed and holds a recognisable pattern: a cb read
	 * past the end of slot 0 then lands in slot 1's bytes rather than in
	 * zeroed BSS, which is what makes the defect observable here. */
	alp_gpio_t *a = alp_gpio_open(0u);
	alp_gpio_t *b = alp_gpio_open(1u);
	zassert_not_null(a, "delegated proxy open of pin 0 failed");
	zassert_not_null(b, "delegated proxy open of pin 1 failed");

	g_fired       = 0;
	g_seen_handle = NULL;
	g_seen_user   = NULL;

	zassert_equal(alp_gpio_configure(a, ALP_GPIO_INPUT, ALP_GPIO_PULL_NONE),
	              ALP_OK,
	              "configure pin 0 as input failed");
	zassert_equal(alp_gpio_irq_enable(a, ALP_GPIO_EDGE_RISING, proxy_cb, (void *)0x1234),
	              ALP_OK,
	              "irq_enable(RISING) on a delegated pin failed");

	/* Drive the edge on the underlying emulated port.  pin_id 0 resolves
	 * to <&gpio_emul0 0> through the alp,pin-array node in this
	 * directory's board overlay -- the same mapping alp_z_gpio_resolve()
	 * walks. */
	const struct device *port = DEVICE_DT_GET(DT_NODELABEL(gpio_emul0));
	zassert_true(device_is_ready(port), "gpio_emul0 not ready");
	zassert_ok(gpio_emul_input_set(port, 0, 0));
	zassert_ok(gpio_emul_input_set(port, 0, 1));
	k_msleep(10);

	zassert_equal(g_fired, 1, "cb did not fire exactly once on the armed rising edge");
	zassert_equal(g_seen_handle,
	              a,
	              "cb fired with the wrong handle: the delegated path recovered an owner "
	              "by CONTAINER_OF on a proxy sidecar instead of the real handle");
	zassert_equal(g_seen_user, (void *)0x1234, "user cookie did not survive the delegation");

	zassert_equal(alp_gpio_irq_disable(a), ALP_OK, "irq_disable failed");
	alp_gpio_close(a);
	alp_gpio_close(b);
}

#endif /* CONFIG_ALP_SDK_GPIO_CC3501E_PROXY */
