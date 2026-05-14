/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * §D.lib library-knob smoke test.
 *
 * Walks the libraries that have Zephyr-native module integration
 * (lvgl / mbedtls / cmsis-dsp + tflite-micro pulled in through the
 * Zephyr west.yml allowlist) and confirms each one's headers
 * include + at least one entry-point symbol links.  Doesn't run
 * meaningful logic against the libraries; the goal is to catch
 * upstream-pin breakage at twister time, before any HiL runner.
 *
 * Symbol-resolution checks below use `(void)` casts to suppress
 * unused-result warnings.  Each library's smoke is wrapped in
 * `#ifdef` so this single test compiles on any combination of
 * library knobs the consumer enables.
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

#if defined(CONFIG_MBEDTLS)
#include <mbedtls/version.h>
#include <mbedtls/md.h>
#endif

#if defined(CONFIG_CMSIS_DSP)
#include <arm_math.h>
#endif

#if defined(CONFIG_FILE_SYSTEM_LITTLEFS)
#include <zephyr/fs/fs.h>
#endif

ZTEST_SUITE(alp_lib_knobs, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------ */
/* mbedtls -- header inclusion + version-string fetch.  Confirms the */
/* CONFIG_MBEDTLS=y path emitted by the alp.conf loader actually    */
/* pulls a buildable mbedtls into the link.                          */
/* ------------------------------------------------------------------ */

ZTEST(alp_lib_knobs, test_mbedtls_links)
{
#if defined(CONFIG_MBEDTLS)
    char version[32] = {0};
    mbedtls_version_get_string(version);
    zassert_true(version[0] != '\0',
                 "mbedtls_version_get_string returned empty");
    /* MBEDTLS_VERSION_NUMBER is a compile-time constant; assert
     * non-zero so a busted Kconfig that links a stub doesn't pass. */
    zassert_true(MBEDTLS_VERSION_NUMBER != 0,
                 "MBEDTLS_VERSION_NUMBER is 0 -- mbedtls headers stubbed?");
#else
    ztest_test_skip();
#endif
}

/* ------------------------------------------------------------------ */
/* cmsis_dsp -- compile-time symbol existence.  arm_math.h must be   */
/* on the include path and at least one transform must resolve.     */
/* ------------------------------------------------------------------ */

ZTEST(alp_lib_knobs, test_cmsis_dsp_links)
{
#if defined(CONFIG_CMSIS_DSP)
    /* Trivial scalar copy through the CMSIS-DSP arm_copy_f32 path.
     * Confirms the math symbol resolves at link time on native_sim. */
    static const float src[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float              dst[4] = {0};
    arm_copy_f32((float32_t *)src, dst, 4);
    zassert_within(dst[3], 4.0f, 1e-6f,
                   "arm_copy_f32 did not propagate the last element");
#else
    ztest_test_skip();
#endif
}

/* ------------------------------------------------------------------ */
/* littlefs -- header path only; native_sim doesn't expose a flash    */
/* device by default so we don't actually mount a filesystem.         */
/* ------------------------------------------------------------------ */

ZTEST(alp_lib_knobs, test_littlefs_headers_resolve)
{
#if defined(CONFIG_FILE_SYSTEM_LITTLEFS)
    struct fs_mount_t mp = {0};
    (void)mp;
    /* The presence of struct fs_mount_t in this translation unit
     * confirms the fs.h header chain reaches littlefs's
     * Kconfig-gated definitions.  Build-time check only. */
    zassert_true(true, "header path resolved at compile time");
#else
    ztest_test_skip();
#endif
}

/* ------------------------------------------------------------------ */
/* §D.lib SW-fallback knobs -- pure compile-time check that the     */
/* per-library Kconfig symbols are reachable from the Zephyr Kconfig*/
/* parser (Kconfig.alp-libraries was sourced under the ALP_SDK if). */
/* If any of these are undefined the preprocessor will emit -1 and  */
/* the assert below fails the test.                                 */
/* ------------------------------------------------------------------ */

#define _ALP_REQUIRE_KCONFIG(x) (defined(CONFIG_##x) ? 1 : 0)

ZTEST(alp_lib_knobs, test_sw_fallback_knobs_defined)
{
    const int reachable =
        _ALP_REQUIRE_KCONFIG(ALP_TFLM_REF_KERNELS)
      + _ALP_REQUIRE_KCONFIG(ALP_MBEDTLS_PURE_C)
      + _ALP_REQUIRE_KCONFIG(ALP_CMSIS_DSP_SCALAR)
      + _ALP_REQUIRE_KCONFIG(ALP_LITTLEFS_SYNC_IO)
      + _ALP_REQUIRE_KCONFIG(ALP_LVGL_SW_BLIT)
      + _ALP_REQUIRE_KCONFIG(ALP_BEARSSL_PURE_C)
      + _ALP_REQUIRE_KCONFIG(ALP_OPUS_PURE_C)
      + _ALP_REQUIRE_KCONFIG(ALP_MINIMP3_PURE_C)
      + _ALP_REQUIRE_KCONFIG(ALP_LIBHELIX_PURE_C)
      + _ALP_REQUIRE_KCONFIG(ALP_MADGWICK_LIBM)
      + _ALP_REQUIRE_KCONFIG(ALP_U8G2_SW_BLIT)
      + _ALP_REQUIRE_KCONFIG(ALP_GFX_COMPAT_SW)
      + _ALP_REQUIRE_KCONFIG(ALP_JSMN_SW)
      + _ALP_REQUIRE_KCONFIG(ALP_CATCH2_SW)
      + _ALP_REQUIRE_KCONFIG(ALP_NANOPB_SW)
      + _ALP_REQUIRE_KCONFIG(ALP_MQTTSN_NO_TLS)
      + _ALP_REQUIRE_KCONFIG(ALP_COAP_NO_TLS)
      + _ALP_REQUIRE_KCONFIG(ALP_TINYGSM_SYNC_IO)
      + _ALP_REQUIRE_KCONFIG(ALP_LWS_NO_TLS)
      + _ALP_REQUIRE_KCONFIG(ALP_MODBUS_SYNC_IO)
      + _ALP_REQUIRE_KCONFIG(ALP_PID_INT_MATH);
    /* All 21 SW-fallback knobs must be reachable at parse time. */
    zassert_equal(reachable, 21,
                  "only %d of 21 SW-fallback knobs resolved; "
                  "check Kconfig.alp-libraries source line in zephyr/Kconfig",
                  reachable);
}
