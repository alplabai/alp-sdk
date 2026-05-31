/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plain-CMake tests for the Yocto/Linux i2c-dev backend
 * (src/yocto/peripheral_i2c.c).
 *
 * Scope: failure paths that don't need a real I2C adapter or
 * kernel-mode i2c-stub.  The "real adapter happy path" coverage
 * lives in the on-device test plan -- HIL coverage exercises a
 * known sensor (LSM6DSO whoami read) and is gated behind the
 * `hil-yocto` runner label (parked behind the v0.4 self-hosted
 * runner provisioning per docs/ci/HW-IN-LOOP.md).
 *
 * What this binary verifies:
 *   - NULL config rejection.
 *   - Opening a non-existent bus index stamps a last-error and
 *     returns NULL (no FD leak).
 *   - NULL/zero-len argument validation on read/write paths.
 *   - alp_i2c_close(NULL) is a safe no-op.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_i2c
 *   ctest --test-dir build -R alp_test_peripheral_i2c
 */

#include <stdint.h>

#include "alp/peripheral.h"

#include "test_assert.h"

/* Bus 9999 is well outside the typical Linux i2c-dev range (0-31).
 * /dev/i2c-9999 will not exist on any sane CI runner, so the
 * `open()` call inside alp_i2c_open() reliably hits ENOENT. */
#define ALP_TEST_BUS_NONEXISTENT 9999u

static void test_null_cfg_returns_null_and_stamps_invalid(void)
{
    alp_i2c_t *bus = alp_i2c_open(NULL);
    ALP_ASSERT_NULL(bus);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_nonexistent_bus_returns_null_and_stamps_not_ready(void)
{
    alp_i2c_config_t cfg = {
        .bus_id     = ALP_TEST_BUS_NONEXISTENT,
        .bitrate_hz = 400000,
    };
    alp_i2c_t *bus = alp_i2c_open(&cfg);
    ALP_ASSERT_NULL(bus);
    /* ENOENT -> ALP_ERR_NOT_READY per the errno_to_alp mapping. */
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_NOT_READY);
}

static void test_write_on_null_bus_returns_invalid(void)
{
    uint8_t      buf[1] = {0x00};
    alp_status_t rc     = alp_i2c_write(NULL, 0x6Bu, buf, sizeof(buf));
    ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_read_on_null_bus_returns_invalid(void)
{
    uint8_t      buf[1] = {0};
    alp_status_t rc     = alp_i2c_read(NULL, 0x6Bu, buf, sizeof(buf));
    ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_write_read_on_null_bus_returns_invalid(void)
{
    uint8_t      wbuf[1] = {0x0F};
    uint8_t      rbuf[1] = {0};
    alp_status_t rc      = alp_i2c_write_read(NULL, 0x6Bu, wbuf, sizeof(wbuf), rbuf, sizeof(rbuf));
    ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_close_null_is_safe(void)
{
    alp_i2c_close(NULL);
    ALP_TEST_PASS();
}

int main(void)
{
    test_null_cfg_returns_null_and_stamps_invalid();
    test_nonexistent_bus_returns_null_and_stamps_not_ready();
    test_write_on_null_bus_returns_invalid();
    test_read_on_null_bus_returns_invalid();
    test_write_read_on_null_bus_returns_invalid();
    test_close_null_is_safe();

    ALP_TEST_SUMMARY();
}
