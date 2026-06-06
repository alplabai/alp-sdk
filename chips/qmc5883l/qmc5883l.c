/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * QST QMC5883L magnetometer driver.  See <alp/chips/qmc5883l.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/qmc5883l.h"

#define QMC5883L_CTRL1_CONTINUOUS 0x01u
#define QMC5883L_CTRL1_ODR_200HZ  0x0Cu
#define QMC5883L_CTRL1_RNG_2G     0x00u
#define QMC5883L_CTRL1_OSR_512    0x00u

static alp_status_t qmc5883l_write_reg(qmc5883l_t *dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
}

alp_status_t qmc5883l_init(qmc5883l_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (i2c_addr == 0) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->bus  = bus;
    dev->addr = i2c_addr;

    /* Per QST app-note: write 0x01 to SET/RESET, then CTRL1 for run mode. */
    alp_status_t s = qmc5883l_write_reg(dev, QMC5883L_REG_SET_RESET, 0x01);
    if (s != ALP_OK) return s;
    s = qmc5883l_write_reg(dev, QMC5883L_REG_CTRL1,
                           QMC5883L_CTRL1_CONTINUOUS | QMC5883L_CTRL1_ODR_200HZ |
                               QMC5883L_CTRL1_RNG_2G | QMC5883L_CTRL1_OSR_512);
    if (s != ALP_OK) return s;

    dev->initialised = true;
    return ALP_OK;
}

alp_status_t qmc5883l_read_axes(qmc5883l_t *dev, qmc5883l_axes_t *axes_out)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (axes_out == NULL) return ALP_ERR_INVAL;

    uint8_t reg    = QMC5883L_REG_DATA_X_LO;
    uint8_t buf[6] = {0};
    alp_status_t s = alp_i2c_write_read(dev->bus, dev->addr, &reg, 1, buf, sizeof(buf));
    if (s != ALP_OK) return s;
    axes_out->x = (int16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
    axes_out->y = (int16_t)((uint16_t)buf[2] | ((uint16_t)buf[3] << 8));
    axes_out->z = (int16_t)((uint16_t)buf[4] | ((uint16_t)buf[5] << 8));
    return ALP_OK;
}

void qmc5883l_deinit(qmc5883l_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
}
