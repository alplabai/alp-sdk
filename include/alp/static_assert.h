/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file static_assert.h
 * @brief Compile-time assertion macro that works in C and C++ headers.
 *
 * Public headers pin wire / register layouts with compile-time size
 * and offset checks.  `_Static_assert` is a C11 keyword that C++ does
 * not have (C++11 spells it `static_assert`), and pre-C11 C has
 * neither, so a bare `_Static_assert` in a header breaks every C++
 * translation unit and every `-std=c99` consumer.  Use
 * @ref ALP_STATIC_ASSERT instead.
 */

#ifndef ALP_STATIC_ASSERT_H
#define ALP_STATIC_ASSERT_H

#if defined(__cplusplus)
/**
 * @brief Assert @p cond at compile time (file or block scope).
 *
 * Expands to `static_assert` under C++11 or later, to `_Static_assert`
 * under C11 or later, and to a harmless repeatable declaration on
 * older C (the check is skipped there).  Write a trailing semicolon.
 *
 * @param cond  Integer constant expression that must be non-zero.
 * @param msg   String literal diagnostic shown when @p cond is zero.
 */
#define ALP_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define ALP_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#define ALP_STATIC_ASSERT(cond, msg) extern int alp_static_assert_unused_
#endif

#endif /* ALP_STATIC_ASSERT_H */
