/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * V2N supervisor singleton -- see v2n_supervisor.h for the contract.
 *
 * Compiled in only when CONFIG_ALP_SDK_V2N_SUPERVISOR=y.  The
 * acquire / release helpers are still declared on every build (the
 * !V2N stub at the bottom returns NOSUPPORT) so peripheral backends
 * can dispatch unconditionally.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "alp/peripheral.h"
#include "v2n_supervisor.h"

#if defined(CONFIG_ALP_SDK_V2N_SUPERVISOR)

#ifndef CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_BUS_ID
#define CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_BUS_ID  (-1)
#endif
#ifndef CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BUS_ID
#define CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BUS_ID  (-1)
#endif
#ifndef CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_ADDR
#define CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_ADDR    GD32G553_BRIDGE_DEFAULT_I2C_ADDR
#endif
#ifndef CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_FREQ_HZ
#define CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_FREQ_HZ 10000000
#endif
#ifndef CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BITRATE_HZ
#define CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BITRATE_HZ 400000
#endif

static struct {
    bool             tried_init;
    alp_status_t     init_status;
    alp_spi_t       *spi;
    alp_i2c_t       *i2c;
    gd32g553_t       ctx;
    struct k_mutex   lock;
} g_v2n;

static int v2n_supervisor_sys_init(void)
{
    k_mutex_init(&g_v2n.lock);
    return 0;
}
SYS_INIT(v2n_supervisor_sys_init, POST_KERNEL,
         CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

static alp_status_t try_init_locked(void)
{
    if (g_v2n.tried_init) return g_v2n.init_status;
    g_v2n.tried_init = true;
    g_v2n.init_status = ALP_ERR_NOT_READY;

#if (CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_BUS_ID >= 0)
    g_v2n.spi = alp_spi_open(&(alp_spi_config_t){
        .bus_id        = (uint32_t)CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_BUS_ID,
        .freq_hz       = (uint32_t)CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_FREQ_HZ,
        .mode          = ALP_SPI_MODE_0,
        .bits_per_word = 8u,
        /* CS routing comes from the board's spi-controller DT node
         * (`cs-gpios` on the SPI bus + DT alias on the supervisor
         * chip-select).  alp_spi_open() resolves it via the standard
         * Zephyr device path. */
    });
    /* A failed SPI open is non-fatal -- the I2C management path may
     * still come up.  gd32g553_init() requires only one of the two
     * transports. */
#endif

#if (CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BUS_ID >= 0)
    g_v2n.i2c = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = (uint32_t)CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BUS_ID,
        .bitrate_hz = (uint32_t)CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BITRATE_HZ,
    });
#endif

    if (g_v2n.spi == NULL && g_v2n.i2c == NULL) {
        /* Neither bus opened (either both Kconfig bus IDs are -1, or
         * both alp_*_open calls failed).  The supervisor is dormant. */
        g_v2n.init_status = ALP_ERR_NOT_READY;
        return g_v2n.init_status;
    }

    const alp_status_t s = gd32g553_init(&g_v2n.ctx, g_v2n.spi, g_v2n.i2c,
                                         (uint8_t)CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_ADDR);
    if (s != ALP_OK) {
        /* Tear the bus handles back down -- a failed handshake means
         * we won't issue further bridge calls, and leaving the buses
         * open would pin pool slots that other code could use. */
        if (g_v2n.spi != NULL) { alp_spi_close(g_v2n.spi); g_v2n.spi = NULL; }
        if (g_v2n.i2c != NULL) { alp_i2c_close(g_v2n.i2c); g_v2n.i2c = NULL; }
        g_v2n.init_status = s;
        return s;
    }

    g_v2n.init_status = ALP_OK;
    return ALP_OK;
}

alp_status_t alp_z_v2n_supervisor_acquire(gd32g553_t **ctx_out)
{
    if (ctx_out == NULL) return ALP_ERR_INVAL;
    *ctx_out = NULL;

    k_mutex_lock(&g_v2n.lock, K_FOREVER);
    const alp_status_t s = try_init_locked();
    if (s != ALP_OK) {
        k_mutex_unlock(&g_v2n.lock);
        return s;
    }
    *ctx_out = &g_v2n.ctx;
    return ALP_OK;
}

void alp_z_v2n_supervisor_release(void)
{
    /* Release is a no-op if init was never attempted (the mutex is
     * still held only by callers of a successful acquire). */
    k_mutex_unlock(&g_v2n.lock);
}

#else  /* !CONFIG_ALP_SDK_V2N_SUPERVISOR -- stubs let backends compile unconditionally. */

alp_status_t alp_z_v2n_supervisor_acquire(gd32g553_t **ctx_out)
{
    if (ctx_out != NULL) *ctx_out = NULL;
    return ALP_ERR_NOSUPPORT;
}

void alp_z_v2n_supervisor_release(void)
{
    /* Nothing to release. */
}

#endif  /* CONFIG_ALP_SDK_V2N_SUPERVISOR */
