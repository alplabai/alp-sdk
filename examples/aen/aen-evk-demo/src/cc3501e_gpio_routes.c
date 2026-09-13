/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * CC3501E GPIO-proxy route table for aen-evk-demo -- the THREE proxied pads
 * this app drives, and no more.
 *
 * WHAT THIS IS. A strong override of the WEAK cc3501e_gpio_routes[] /
 * cc3501e_gpio_route_count in src/backends/gpio/cc3501e_proxy_routes_weak.c.
 * It maps a PORTABLE E1M GPIO pin id -- what alp_gpio_open() takes -- to the
 * RAW CC3501E GPIO index the inter-chip bridge drives, so an alp_gpio_* call
 * on a proxied pad is routed over the bridge while every Alif-side pin still
 * delegates to the platform driver. Compiled unconditionally; only consulted
 * when CONFIG_ALP_SDK_GPIO_CC3501E_PROXY is set (it is -- see prj.conf).
 *
 * WHY THIS ONE IS HAND-WRITTEN WHEN THE OTHERS ARE GENERATED, and how it is
 * kept from drifting anyway. scripts/gen_cc3501e_gpio_routes.py generates this
 * file for aen-cc3501e-bringup, aen-cc3501e-companion-tour and
 * aen-cc3501e-gpio; it discovers its targets by looking for a board.yaml
 * beside a prj.conf that enables the proxy. aen-evk-demo has NO board.yaml --
 * it is a standalone Zephyr app by design (see CMakeLists.txt), so there is no
 * composed route table for the generator to resolve against and it correctly
 * skips this app.
 *
 * Hand-maintaining a full nine-pad table here is exactly the triplication
 * issue #1859 removed, so this file does not carry one: it declares only the
 * pads phases 9 and 11 actually drive. Every entry is still pinned to
 * metadata rather than trusted -- tests/scripts/test_aen_cc3501e_routes.py
 * checks all of them against metadata/e1m_modules/aen/from-cc3501e.tsv, the
 * same source the generator resolves through, and asserts the table stays a
 * SUBSET of the full map. Add a pad here only alongside that test.
 *
 * REVISION SCOPE. IO20 -> GPIO_26 holds on BOTH AEN module revisions (it is
 * not one of the pads hw-revisions.yaml moves), so this table needs no
 * per-revision variant. IO21 -- the SD mux SELECT -- is deliberately ABSENT:
 * on r2 it is physically open and on r1 driving it would contend with the P18
 * header jumper. See phase 9's header in src/main.c.
 *
 * IO8 AND IO13 -- the I2S mux ENABLE and SELECT phase 11 drives -- are BOTH
 * CC3501E-owned per metadata/e1m_modules/E1M-AEN801.yaml's pad_routes (IO8 ->
 * dispatch_pin 30, IO13 -> dispatch_pin 13), confirmed against
 * from-cc3501e.tsv (AG33 IO8 GPIO30; E3 IO13 GPIO13). NOTE: this contradicts
 * examples/aen/aen-i2s-amp-alif/README.md's "EN = IO8 -> Alif P7.1" claim and
 * metadata/boards/e1m-evk.yaml's matching EVK_PIN_I2S_MUX_EN doc string --
 * both are stale prose left over from before the SoM's pad_routes were
 * finalised; the pad_routes table (what the generator and this app's GPIO
 * dispatch actually resolve through) is the one both phases 9 and 11 trust.
 */

#include <stddef.h>

#include <alp/chips/cc3501e.h>
#include <alp/e1m_pinout.h>

const cc3501e_gpio_route_t cc3501e_gpio_routes[] = {
	{ ALP_E1M_GPIO_IO20, 26u }, /* SDIO 74LVC157 /E; drive low to enable mux. */
	{ ALP_E1M_GPIO_IO8, 30u },  /* I2S0 74LVC157 /E; drive low to enable mux. */
	{ ALP_E1M_GPIO_IO13, 13u }, /* I2S0 74LVC157 S; 0 = TAS2563 amps, 1 = M.2 E-key I2S. */
};

const size_t cc3501e_gpio_route_count =
    sizeof(cc3501e_gpio_routes) / sizeof(cc3501e_gpio_routes[0]);
