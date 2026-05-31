/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * doctest configuration header for the Alp SDK's embedded targets.
 *
 * doctest (https://github.com/doctest/doctest) is a single-header
 * C++ test framework.  Apps using it under board.yaml's
 * `libraries: [doctest]` declaration get this profile force-
 * included so the test runner doesn't pull in features that
 * don't exist on Cortex-M.
 *
 * Test-only -- doctest doesn't ship in production firmware.
 * Profile primarily disables POSIX-dependent features so the
 * test runner builds clean against newlib-nano / picolibc.
 *
 * Consumers wanting different settings drop their own
 * doctest config defines before #include <doctest/doctest.h>.
 */

#ifndef ALP_DOCTEST_CONFIG_H_
#define ALP_DOCTEST_CONFIG_H_

/* No POSIX signal handlers -- M-class targets don't have them
 * and the test runner's signal-catching path won't compile. */
#define DOCTEST_CONFIG_NO_POSIX_SIGNALS

/* No multithreading -- the SDK's tests are single-threaded for
 * deterministic ordering.  Cuts out doctest's mutex layer. */
#define DOCTEST_CONFIG_NO_MULTITHREADING

/* Use std::nullptr_t fallback instead of pulling in <type_traits>
 * if the C++ standard library headers aren't fully present. */
#define DOCTEST_CONFIG_NO_INCLUDE_TYPETRAITS

/* The SDK's host-side test build defines this -- the on-target
 * build (rare) leaves it off so tests can use SUBCASE without
 * the long-name registration cost. */
/* #define DOCTEST_CONFIG_NO_SHORT_MACRO_NAMES */

#endif /* ALP_DOCTEST_CONFIG_H_ */
