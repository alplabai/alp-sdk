/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file common.h
 * @brief Cross-class reset for `alp/testing` test doubles.
 *
 * Every class double keeps its injected state in a per-class instance
 * table (src/testing/instance_table.c) keyed by the class's public
 * instance id, plus the shared virtual clock (`<alp/testing/clock.h>`).
 * @ref alp_testing_reset_all wipes all of it in one call so a test
 * suite can start each case from a clean slate without caring how many
 * classes are compiled in.
 */

#ifndef ALP_TESTING_COMMON_H
#define ALP_TESTING_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset every `alp/testing` double to its initial state.
 *
 * Resets the virtual clock (@ref alp_testing_clock_reset) and wipes
 * every registered class instance table (injected levels, armed
 * edges, callbacks, counters -- everything a test may have injected
 * or that a prior open() populated).  Call this in a test fixture's
 * setup/teardown so cases don't leak state into one another.
 */
void alp_testing_reset_all(void);

/**
 * @brief One-shot bus fault a controller-mode `alp/testing` double
 *        injects for its NEXT transfer (`<alp/testing/i2c.h>`,
 *        `<alp/testing/spi.h>`).
 *
 * Shared by every bus-shaped class double (I2C and SPI so far) so the
 * fault model -- and its mapping onto @ref alp_status_t -- is one
 * concept, not a per-class reinvention:
 *   - @ref ALP_TESTING_FAULT_NACK    -- the simulated device does not
 *     acknowledge at all; nothing is transferred (I2C: address-phase
 *     NACK; SPI: modelled as the target never responding, e.g. a
 *     mis-wired/absent chip-select). Surfaces as @ref ALP_ERR_IO.
 *   - @ref ALP_TESTING_FAULT_SHORT   -- the device drops off partway
 *     through: only the fault's `short_len` bytes are actually
 *     transferred (captured on write / filled on read) before the bus
 *     error is raised. Surfaces as @ref ALP_ERR_IO, distinguishable
 *     from NACK by the partial data a test can read back.
 *   - @ref ALP_TESTING_FAULT_TIMEOUT -- the transfer never completes.
 *     Neither of these classes' ops take a caller timeout (unlike
 *     `<alp/testing/uart.h>`'s virtual-clock-resolved read()), so this
 *     is modelled as an immediate @ref ALP_ERR_TIMEOUT with nothing
 *     transferred, rather than a clock-driven wait.
 *
 * A fault armed via `..._fail_next()` fires exactly once (the next
 * transfer that touches the faulted bus/address), then disarms itself
 * automatically -- a test does not need to clear it back out.
 */
typedef enum {
	ALP_TESTING_FAULT_NACK = 0, /**< No acknowledgement; nothing transferred. */
	ALP_TESTING_FAULT_SHORT,    /**< Only `short_len` bytes transferred, then a bus error. */
	ALP_TESTING_FAULT_TIMEOUT,  /**< Transfer never completes. */
} alp_testing_bus_fault_t;

#ifdef __cplusplus
}
#endif

#endif /* ALP_TESTING_COMMON_H */
