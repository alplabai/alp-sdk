/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file gpio.h
 * @brief Injection API for the GPIO test double (priority-255 backend).
 *
 * `src/backends/gpio/testing_drv.c` registers a `silicon_ref="*"`
 * backend at priority 255 (see @ref ALP_BACKEND_REGISTER), so with
 * `CONFIG_ALP_SDK_TESTING_GPIO=y` it wins @ref alp_backend_select for
 * every pin id and the portable `<alp/peripheral.h>` `alp_gpio_*` API
 * rides on it transparently -- no real or emulated GPIO controller
 * needed.  This header is the test-side control surface: it injects
 * input levels and edges into a pin and reads back what the app under
 * test drove as output.
 *
 * Every function keys off the same @p pin_id the app passes to
 * @ref alp_gpio_open, and every function is create-on-first-touch --
 * a test may inject a level or arm an edge BEFORE the app opens the
 * pin, so power-on/reset-strap scenarios are expressible.  The two
 * pure read-back accessors (@ref alp_testing_gpio_get_output,
 * @ref alp_testing_gpio_write_count) are the exception: they report
 * on a pin id that has been touched at least once (by an injector
 * call or by `alp_gpio_open`), and fail with @ref ALP_ERR_INVAL for
 * one that never has -- there is nothing yet to read back.
 */

#ifndef ALP_TESTING_GPIO_H
#define ALP_TESTING_GPIO_H

#include <stdbool.h>
#include <stdint.h>

#include <alp/peripheral.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set the virtual input level an open (or not-yet-open) pin
 *        reads back from @ref alp_gpio_read.
 *
 * @param[in] pin_id  The same id the app passes to @ref alp_gpio_open.
 * @param[in] level   Level @ref alp_gpio_read observes from now on.
 *
 * @return ALP_OK on success; ALP_ERR_NOMEM if the instance table is full.
 */
alp_status_t alp_testing_gpio_set_input(uint32_t pin_id, bool level);

/**
 * @brief Inject an edge transition on @p pin_id right now.
 *
 * Sets the pin's virtual input level to match the edge direction
 * (RISING -> high, FALLING -> low) and, if an
 * @ref alp_gpio_irq_enable armed edge is pending AND matches
 * @p edge (an armed @ref ALP_GPIO_EDGE_BOTH matches either
 * @ref ALP_GPIO_EDGE_RISING or @ref ALP_GPIO_EDGE_FALLING), invokes
 * the registered callback synchronously, on the calling thread,
 * before this function returns.  A closed handle's armed edge is
 * cleared by `testing_drv.c`'s close(), so injecting on a pin id
 * whose handle has since closed is a documented no-op (no
 * use-after-close callback).
 *
 * @param[in] pin_id  The same id the app passes to @ref alp_gpio_open.
 * @param[in] edge    ALP_GPIO_EDGE_RISING or ALP_GPIO_EDGE_FALLING.
 *                     ALP_GPIO_EDGE_NONE/ALP_GPIO_EDGE_BOTH are invalid
 *                     as an injected stimulus (a real line transitions
 *                     one way at a time).
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if @p edge is not RISING or
 *         FALLING; ALP_ERR_NOMEM if the instance table is full.
 */
alp_status_t alp_testing_gpio_edge(uint32_t pin_id, alp_gpio_edge_t edge);

/**
 * @brief Schedule an @ref alp_testing_gpio_edge injection for a future
 *        virtual-clock timestamp.
 *
 * The injection fires the moment a subsequent
 * @ref alp_testing_clock_advance_ms carries the virtual clock's "now"
 * to `>= at_ms` -- synchronously, in the caller-of-advance's thread,
 * exactly like a direct @ref alp_testing_gpio_edge call at that
 * instant. Deferred, not immediate: this call itself never fires the
 * callback.
 *
 * @param[in] pin_id  The same id the app passes to @ref alp_gpio_open.
 * @param[in] at_ms   Virtual-clock timestamp (@ref alp_testing_clock_now_ms)
 *                     at which the edge fires.
 * @param[in] edge    ALP_GPIO_EDGE_RISING or ALP_GPIO_EDGE_FALLING.
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if @p edge is not RISING or
 *         FALLING; ALP_ERR_NOMEM if the event queue or instance table
 *         is full.
 */
alp_status_t alp_testing_gpio_edge_at(uint32_t pin_id, uint64_t at_ms, alp_gpio_edge_t edge);

/**
 * @brief Read back the level the app under test last drove with
 *        @ref alp_gpio_write.
 *
 * @param[in]  pin_id  The same id the app passes to @ref alp_gpio_open.
 * @param[out] level   Receives the last-written level.  Must be non-NULL.
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if @p level is NULL or
 *         @p pin_id has never been touched (no open, no injection).
 */
alp_status_t alp_testing_gpio_get_output(uint32_t pin_id, bool *level);

/**
 * @brief Read the number of @ref alp_gpio_write calls observed on
 *        @p pin_id since the last @ref alp_testing_reset_all.
 *
 * @param[in]  pin_id  The same id the app passes to @ref alp_gpio_open.
 * @param[out] n       Receives the write count.  Must be non-NULL.
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if @p n is NULL or
 *         @p pin_id has never been touched (no open, no injection).
 */
alp_status_t alp_testing_gpio_write_count(uint32_t pin_id, uint32_t *n);

#ifdef __cplusplus
}
#endif

#endif /* ALP_TESTING_GPIO_H */
