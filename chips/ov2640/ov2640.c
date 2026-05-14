/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * OmniVision OV2640 2 MP CMOS image sensor driver (SCCB/I²C side).
 * See <alp/chips/ov2640.h> for the public API + scope split.
 *
 * v0.5: chip-ID verify + soft reset + setter stash.  Vendor-init
 * register tables for each (resolution × format) pair land in
 * follow-up commits once the maintainer adds the reference init
 * script to the internal design archive.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/ov2640.h"

#define OV2640_REG_DSP_RESET 0x12u /* bank DSP — bit 7 = soft reset. */

static alp_status_t ov2640_select_bank(ov2640_t *dev, uint8_t bank)
{
    uint8_t buf[2] = {OV2640_REG_BANK_SEL, bank};
    return alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
}

static alp_status_t ov2640_read_reg(ov2640_t *dev, uint8_t reg, uint8_t *val)
{
    return alp_i2c_write_read(dev->bus, dev->addr, &reg, 1, val, 1);
}

static alp_status_t ov2640_write_reg(ov2640_t *dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
}

alp_status_t ov2640_init(ov2640_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (i2c_addr == 0) return ALP_ERR_INVAL;

    memset(dev, 0, sizeof(*dev));
    dev->bus  = bus;
    dev->addr = i2c_addr;

    alp_status_t s = ov2640_select_bank(dev, OV2640_BANK_SENSOR);
    if (s != ALP_OK) return s;

    uint8_t pidh = 0, pidl = 0;
    s = ov2640_read_reg(dev, OV2640_REG_PIDH, &pidh);
    if (s != ALP_OK) return s;
    s = ov2640_read_reg(dev, OV2640_REG_PIDL, &pidl);
    if (s != ALP_OK) return s;

    uint16_t id = ((uint16_t)pidh << 8) | pidl;
    if (id != OV2640_CHIP_ID) return ALP_ERR_IO;

    dev->res         = OV2640_RES_UXGA;
    dev->fmt         = OV2640_FMT_JPEG;
    dev->initialised = true;
    return ALP_OK;
}

alp_status_t ov2640_read_id(ov2640_t *dev, uint16_t *id_out)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (id_out == NULL) return ALP_ERR_INVAL;

    alp_status_t s = ov2640_select_bank(dev, OV2640_BANK_SENSOR);
    if (s != ALP_OK) return s;
    uint8_t pidh = 0, pidl = 0;
    s = ov2640_read_reg(dev, OV2640_REG_PIDH, &pidh);
    if (s != ALP_OK) return s;
    s = ov2640_read_reg(dev, OV2640_REG_PIDL, &pidl);
    if (s != ALP_OK) return s;
    *id_out = ((uint16_t)pidh << 8) | pidl;
    return ALP_OK;
}

alp_status_t ov2640_soft_reset(ov2640_t *dev)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    alp_status_t s = ov2640_select_bank(dev, OV2640_BANK_DSP);
    if (s != ALP_OK) return s;
    return ov2640_write_reg(dev, OV2640_REG_DSP_RESET, 0x80);
}

alp_status_t ov2640_set_resolution(ov2640_t *dev, ov2640_resolution_t res)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if ((int)res < 0 || (int)res > (int)OV2640_RES_UXGA) return ALP_ERR_INVAL;
    dev->res = res;
    return ALP_ERR_NOSUPPORT; /* Vendor register table pending. */
}

alp_status_t ov2640_set_format(ov2640_t *dev, ov2640_format_t fmt)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if ((int)fmt < 0 || (int)fmt > (int)OV2640_FMT_JPEG) return ALP_ERR_INVAL;
    dev->fmt = fmt;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t ov2640_set_test_pattern(ov2640_t *dev, bool enabled)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    (void)enabled;
    return ALP_ERR_NOSUPPORT;
}

void ov2640_deinit(ov2640_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
}
