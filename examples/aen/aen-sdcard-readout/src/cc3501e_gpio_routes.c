/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * CC3501E GPIO-proxy route table for aen-sdcard-readout -- the ONE proxied pad
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
 * beside a prj.conf that enables the proxy. aen-sdcard-readout has NO
 * board.yaml -- it is a standalone Zephyr app by design (see CMakeLists.txt),
 * so there is no composed route table for the generator to resolve against
 * and it correctly skips this app. Copied from examples/aen/aen-evk-demo's
 * table of the same name, the silicon-proven origin of this one entry.
 *
 * Hand-maintaining a full nine-pad table here is exactly the triplication
 * issue #1859 removed, so this file does not carry one: it declares the SINGLE
 * pad this app actually drives -- the SD card's SDIO mux ENABLE. That entry is
 * still pinned to metadata rather than trusted -- tests/scripts/
 * test_aen_cc3501e_routes.py checks it against metadata/e1m_modules/aen/
 * from-cc3501e.tsv, the same source the generator resolves through, and
 * asserts it stays a SUBSET of the full map. Add a pad here only alongside
 * that test.
 *
 * REVISION SCOPE. IO20 -> GPIO_26 holds on BOTH AEN module revisions (it is
 * not one of the pads hw-revisions.yaml moves), so this table needs no
 * per-revision variant. IO21 -- the SD mux SELECT -- is deliberately ABSENT:
 * on r2 it is physically open and on r1 driving it would contend with the P18
 * header jumper. See main()'s mux-enable comment in src/main.c.
 */

#include <stddef.h>

#include <alp/chips/cc3501e.h>
#include <alp/e1m_pinout.h>

const cc3501e_gpio_route_t cc3501e_gpio_routes[] = {
	{ ALP_E1M_GPIO_IO20, 26u }, /* SDIO 74LVC157 /E; drive low to enable mux. */
};

const size_t cc3501e_gpio_route_count =
    sizeof(cc3501e_gpio_routes) / sizeof(cc3501e_gpio_routes[0]);
