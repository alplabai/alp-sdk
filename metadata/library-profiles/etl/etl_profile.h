/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * etl_profile.h for the ALP SDK's embedded targets.
 *
 * ETL (https://github.com/ETLCPP/etl) reads etl_profile.h from
 * the include path BEFORE its own defaults.  This file is what
 * the loader puts in front of the upstream copy when the
 * consumer's alp.yaml lists `etl` under `libraries:`.
 *
 * Invariants we enforce, matching the SDK's no-exceptions /
 * no-heap-on-hot-path / Cortex-M-or-A class targets:
 *
 *   - ETL_NO_STL           : we don't pull in libstdc++ on M-class
 *                            builds; ETL operates standalone.
 *   - ETL_THROW_EXCEPTIONS : disabled.  Failures call etl_error_handler
 *                            (the SDK's default handler logs +
 *                            triggers a fault).
 *   - ETL_TARGET_DEVICE_*  : set per target -- handled by the
 *                            loader's emit_zephyr / emit_cmake
 *                            paths based on the alp.yaml som.sku.
 *   - ETL_CPP17_SUPPORTED  : on by default; the SDK builds with
 *                            -std=c++17 on every supported toolchain.
 *
 * Consumers wanting different ETL settings supply their own
 * etl_profile.h at their app's include root; the loader prefers
 * the app's profile over this one when both exist.
 */

#ifndef ETL_PROFILE_H
#define ETL_PROFILE_H

/* ----------------------------------------------------------------- */
/* Library compatibility -- match the SDK's environment              */
/* ----------------------------------------------------------------- */

/* The SDK builds against newlib-nano / picolibc on M-class targets
 * without the full STL.  ETL's STL-aware paths are off. */
#define ETL_NO_STL

/* Exceptions are off across the SDK's hot-path code.  ETL routes
 * failures through etl_error_handler instead of throwing. */
#define ETL_NO_EXCEPTIONS

/* The SDK targets C++17 minimum (every supported toolchain has it). */
#define ETL_CPP17_SUPPORTED 1
#define ETL_CPP14_SUPPORTED 1
#define ETL_CPP11_SUPPORTED 1

/* No CPU-specific overrides here -- the loader emits
 * ETL_TARGET_DEVICE_ARM_CORTEX_<variant> through compile defines
 * based on the SoM SKU (e.g. cortex_m55 for AEN, cortex_a55 for
 * V2N).  Profile stays portable. */

/* ----------------------------------------------------------------- */
/* Optional defaults -- consumers override in their own profile      */
/* ----------------------------------------------------------------- */

/* Reasonable embedded ceilings -- consumers override if larger
 * containers are needed.  These are *maximum* compile-time sizes;
 * runtime usage is whatever the consumer requests up to this cap. */
#ifndef ETL_MESSAGE_TIMER_MAX_TIMERS
#define ETL_MESSAGE_TIMER_MAX_TIMERS 8
#endif

#ifndef ETL_CALLBACK_TIMER_MAX_TIMERS
#define ETL_CALLBACK_TIMER_MAX_TIMERS 8
#endif

#endif /* ETL_PROFILE_H */
