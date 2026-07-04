/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Static-archive link regression for the backend registry (#368).
 *
 * Unlike the other tests/yocto/ suites, this one links ONLY against the
 * static libalp_sdk.a -- it deliberately compiles NO backend TU of its
 * own.  That is the whole point: on a plain-CMake static link the
 * archive member that carries a class's ALP_BACKEND_REGISTER entry
 * (its sw_fallback) is pure section data that nothing references, so
 * the linker never pulls it, the alp_backends_<class> section stays
 * absent, and the __start_/__stop_ bounds the dispatcher reads go
 * undefined.  Before the ALP_BACKEND_ANCHOR fix this file FAILS TO
 * LINK with e.g. `undefined reference to __start_alp_backends_rtc` for
 * every registry class below; with the anchors it links and runs.
 *
 * The other yocto tests hide this bug by recompiling the backend
 * sources straight into their executables (see the sw_fallback / ALSA
 * blocks in tests/yocto/CMakeLists.txt) -- so they never exercise the
 * bare-archive link model that #368 is about.  This test is the one
 * that regresses it.
 *
 * What it proves:
 *   1. LINK: every reachable registry dispatcher resolves its section
 *      bounds against the bare static archive (the open() calls below
 *      force each dispatcher, hence each __start_/__stop_ pair, into
 *      the link).
 *   2. POPULATED: the anchor actually pulled the section-carrying
 *      member, not merely satisfied the bound symbols -- the portable
 *      catch-all classes report a non-empty registry at runtime.
 *   3. CLASS TABLE: alp_backend_count() links even though this TU
 *      pulls backend.c without pulling any dispatcher on its own
 *      account (the alp_backend_classes self-anchor, #368).
 *
 * Runtime results are otherwise host-dependent (a CI host has no
 * /dev/rtc0 etc.), so the open() calls pass NULL / benign ids and the
 * return values are intentionally ignored -- linking + not crashing is
 * the contract.  No pkg-config gate: every class here ships a portable
 * sw_fallback, so the test runs on any Linux CI host.
 *
 * Build:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_registry_static_link
 *   ctest --test-dir build -R alp_test_registry_static_link
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/adc.h"
#include "alp/audio.h"
#include "alp/backend.h"
#include "alp/ble.h"
#include "alp/can.h"
#include "alp/counter.h"
#include "alp/gpu2d.h"
#include "alp/i2s.h"
#include "alp/iot.h"
#include "alp/pwm.h"
#include "alp/rpc.h"
#include "alp/rtc.h"
#include "alp/security.h"
#include "alp/wdt.h"

#include "test_assert.h"

/*
 * Force every reachable registry dispatcher into the link.  Each call
 * references an alp_<class>_open symbol owned by a dispatcher TU; that
 * TU's ALP_BACKEND_DEFINE_CLASS reads __start_/__stop_alp_backends_<class>,
 * which only resolve because the ALP_BACKEND_ANCHOR in the dispatcher
 * pulled the section-carrying sw_fallback member.  Args are NULL /
 * benign: the dispatchers validate them and return without needing a
 * live device, so this stays host-independent.
 */
static void test_every_dispatcher_links(void)
{
	(void)alp_rtc_open(0u);
	(void)alp_wdt_open(NULL);
	(void)alp_can_open(NULL);
	(void)alp_pwm_open(NULL);
	(void)alp_adc_open(NULL);
	(void)alp_i2s_open(NULL);
	(void)alp_counter_open(NULL);
	(void)alp_audio_in_open(NULL);
	(void)alp_audio_out_open(NULL);
	(void)alp_rpc_open(NULL);
	(void)alp_mqtt_open(NULL);
	(void)alp_wifi_open();
	(void)alp_ble_open();
	(void)alp_gpu2d_open();

	uint8_t rnd[4] = { 0 };
	(void)alp_random_bytes(rnd, sizeof rnd);

	/* Reaching this line means the link succeeded -- the real
	 * assertion is the link itself. */
	ALP_ASSERT_TRUE(1);
}

/*
 * Prove the anchor PULLED the section member (not just resolved the
 * bound symbols): the portable catch-alls register a wildcard backend,
 * so the class registry is non-empty at runtime on any host.
 */
static void test_registry_sections_are_populated(void)
{
	ALP_ASSERT_TRUE(ALP_BACKEND_AVAILABLE(gpu2d));
	ALP_ASSERT_TRUE(ALP_BACKEND_AVAILABLE(rtc));
	ALP_ASSERT_TRUE(ALP_BACKEND_AVAILABLE(wdt));
	ALP_ASSERT_TRUE(ALP_BACKEND_AVAILABLE(can));
	ALP_ASSERT_TRUE(ALP_BACKEND_AVAILABLE(audio));
	ALP_ASSERT_TRUE(ALP_BACKEND_AVAILABLE(ble));
}

/*
 * alp_backend_count() on an unknown class must link and return 0 -- it
 * pulls backend.c (which reads __start_/__stop_alp_backend_classes)
 * without this TU otherwise pulling a dispatcher for that name, so it
 * exercises the alp_backend_classes self-anchor (#368).
 */
static void test_unknown_class_counts_zero(void)
{
	ALP_ASSERT_EQ_INT((int)alp_backend_count("no_such_class"), 0);
	ALP_ASSERT_EQ_INT((int)alp_backend_count(NULL), 0);
}

int main(void)
{
	test_every_dispatcher_links();
	test_registry_sections_are_populated();
	test_unknown_class_counts_zero();
	return 0;
}
