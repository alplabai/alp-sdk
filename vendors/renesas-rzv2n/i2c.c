/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bare-metal Renesas RZ/V2N I2C wrapper for <alp/peripheral.h>.
 *
 * Active when ALP_OS=baremetal + ALP_SOM=v2n + ALP_HAS_RENESAS_FSP=ON.
 * RZ/V2N exposes I2C through Renesas FSP's `r_riic_master` driver;
 * we wrap nine I2C channels (RIIC0..RIIC8) -- E1M-X routes I2C0/I2C1
 * to the carrier and the rest stay SoC-internal.
 *
 * Body gated on ALP_HAS_RENESAS_FSP -- absent the FSP pack, this
 * file is an empty translation unit and stub_backend.c provides
 * the NOSUPPORT defaults.  v0.4 flips ALP_HAS_RENESAS_FSP=ON in
 * CI alongside the FSP pack pull.
 */

#include "alp/peripheral.h"

#if defined(ALP_HAS_RENESAS_FSP)

#include <stddef.h>
#include <stdint.h>

#include "r_riic_master.h"

extern i2c_master_instance_t        g_i2c_master0;
extern i2c_master_instance_t        g_i2c_master1;

static i2c_master_instance_t *const alp_v2n_i2c_drivers[] = {
    &g_i2c_master0,
    &g_i2c_master1,
};

static alp_status_t fsp_to_alp(fsp_err_t err)
{
    switch (err) {
    case FSP_SUCCESS:
        return ALP_OK;
    case FSP_ERR_INVALID_ARGUMENT:
        return ALP_ERR_INVAL;
    case FSP_ERR_IN_USE:
        return ALP_ERR_BUSY;
    case FSP_ERR_TIMEOUT:
        return ALP_ERR_TIMEOUT;
    case FSP_ERR_UNSUPPORTED:
        return ALP_ERR_NOSUPPORT;
    case FSP_ERR_OUT_OF_MEMORY:
        return ALP_ERR_NOMEM;
    default:
        return ALP_ERR_IO;
    }
}

static alp_status_t z_last_error;

alp_i2c_t          *alp_i2c_open(const alp_i2c_config_t *cfg)
{
    if (cfg == NULL) {
        z_last_error = ALP_ERR_INVAL;
        return NULL;
    }
    if (cfg->bus_id >= sizeof(alp_v2n_i2c_drivers) / sizeof(alp_v2n_i2c_drivers[0])) {
        z_last_error = ALP_ERR_OUT_OF_RANGE;
        return NULL;
    }
    i2c_master_instance_t *inst = alp_v2n_i2c_drivers[cfg->bus_id];
    if (inst == NULL) {
        z_last_error = ALP_ERR_NOT_READY;
        return NULL;
    }
    fsp_err_t err = inst->p_api->open(inst->p_ctrl, inst->p_cfg);
    if (err != FSP_SUCCESS) {
        z_last_error = fsp_to_alp(err);
        return NULL;
    }
    return (alp_i2c_t *)inst;
}

alp_status_t alp_i2c_write(alp_i2c_t *bus, uint8_t addr, const uint8_t *data, size_t len)
{
    if (bus == NULL) return ALP_ERR_NOT_READY;
    i2c_master_instance_t *inst = (i2c_master_instance_t *)bus;
    fsp_err_t err = inst->p_api->slaveAddressSet(inst->p_ctrl, addr, I2C_MASTER_ADDR_MODE_7BIT);
    if (err != FSP_SUCCESS) return fsp_to_alp(err);
    return fsp_to_alp(inst->p_api->write(inst->p_ctrl, data, (uint32_t)len, false));
}

alp_status_t alp_i2c_read(alp_i2c_t *bus, uint8_t addr, uint8_t *data, size_t len)
{
    if (bus == NULL) return ALP_ERR_NOT_READY;
    i2c_master_instance_t *inst = (i2c_master_instance_t *)bus;
    fsp_err_t err = inst->p_api->slaveAddressSet(inst->p_ctrl, addr, I2C_MASTER_ADDR_MODE_7BIT);
    if (err != FSP_SUCCESS) return fsp_to_alp(err);
    return fsp_to_alp(inst->p_api->read(inst->p_ctrl, data, (uint32_t)len, false));
}

alp_status_t alp_i2c_write_read(alp_i2c_t *bus, uint8_t addr, const uint8_t *wdata, size_t wlen,
                                uint8_t *rdata, size_t rlen)
{
    if (bus == NULL) return ALP_ERR_NOT_READY;
    alp_status_t s = alp_i2c_write(bus, addr, wdata, wlen);
    if (s != ALP_OK) return s;
    return alp_i2c_read(bus, addr, rdata, rlen);
}

void alp_i2c_close(alp_i2c_t *bus)
{
    if (bus == NULL) return;
    i2c_master_instance_t *inst = (i2c_master_instance_t *)bus;
    (void)inst->p_api->close(inst->p_ctrl);
}

alp_status_t alp_last_error(void)
{
    return z_last_error;
}

#endif /* ALP_HAS_RENESAS_FSP */
