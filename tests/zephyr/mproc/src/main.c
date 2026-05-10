/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smoke tests for <alp/mproc.h> under native_sim.  No mbox device
 * present, no peer core; the wrapper falls back to NOSUPPORT and
 * we verify that contract plus every NULL-arg branch.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"
#include "alp/mproc.h"

ZTEST_SUITE(alp_mproc, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_mproc, test_shmem_open_no_backend_returns_null)
{
    alp_shmem_config_t cfg = {.name = "alp_shmem0", .size = 4096, .cacheable = false};
    alp_shmem_t       *s   = alp_shmem_open(&cfg);
    zassert_is_null(s);
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_mproc, test_shmem_open_null_invalid)
{
    zassert_is_null(alp_shmem_open(NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_mproc, test_shmem_view_null_handle_errors)
{
    void  *base = (void *)0xDEADBEEF;
    size_t size = 999;
    zassert_equal(alp_shmem_view(NULL, &base, &size), ALP_ERR_NOT_READY);
    zassert_is_null(base);
    zassert_equal(size, 0);
    alp_shmem_close(NULL);
}

ZTEST(alp_mproc, test_mbox_open_no_backend_returns_null)
{
    alp_mbox_config_t cfg = {.channel = 0, .peer = ALP_CORE_M55_HE};
    alp_mbox_t       *m   = alp_mbox_open(&cfg);
    zassert_is_null(m);
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_mproc, test_mbox_lifecycle_null_handle_safe)
{
    uint8_t buf[4] = {0};
    zassert_equal(alp_mbox_send(NULL, buf, sizeof(buf), 100), ALP_ERR_NOT_READY);
    zassert_equal(alp_mbox_set_callback(NULL, NULL, NULL), ALP_ERR_NOT_READY);
    alp_mbox_close(NULL);
}

ZTEST(alp_mproc, test_hwsem_open_no_backend_returns_null)
{
    alp_hwsem_t *s = alp_hwsem_open(0);
    zassert_is_null(s);
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_mproc, test_hwsem_lifecycle_null_handle_safe)
{
    zassert_equal(alp_hwsem_try_lock(NULL), ALP_ERR_NOT_READY);
    zassert_equal(alp_hwsem_lock(NULL, 100), ALP_ERR_NOT_READY);
    zassert_equal(alp_hwsem_unlock(NULL), ALP_ERR_NOT_READY);
    alp_hwsem_close(NULL);
}
