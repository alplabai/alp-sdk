/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal scheduling seam behind <alp/testing/clock.h>.  NOT a
 * public header -- class test doubles (e.g. src/backends/gpio/
 * testing_drv.c) call this to defer a stimulus onto the virtual
 * clock; app/test code only ever sees the three functions in
 * <alp/testing/clock.h>.
 */

#ifndef ALP_TESTING_VIRTUAL_CLOCK_INTERNAL_H
#define ALP_TESTING_VIRTUAL_CLOCK_INTERNAL_H

#include <stdint.h>

#include <alp/peripheral.h>

/** One-shot callback fired by alp_testing_clock_advance_ms(). */
typedef void (*alp_testing_clock_event_fn)(void *ctx);

/*
 * Schedule `fn(ctx)` to fire the moment the virtual clock's "now"
 * reaches or passes `at_ms`.  Fires synchronously, in timestamp
 * order relative to any other pending event, from inside a caller's
 * alp_testing_clock_advance_ms() call -- never immediately, even if
 * at_ms is already in the past (advance_ms is what delivers it).
 *
 * Returns ALP_OK, or ALP_ERR_NOMEM if the event queue is full.
 */
alp_status_t alp_testing_clock_schedule(uint64_t at_ms, alp_testing_clock_event_fn fn, void *ctx);

#endif /* ALP_TESTING_VIRTUAL_CLOCK_INTERNAL_H */
