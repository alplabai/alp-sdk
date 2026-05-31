/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * OmniVision OV5640 SCCB-side configuration driver.  See header.
 *
 * The OV5640 uses 16-bit register addressing — every transfer prefixes
 * the data byte(s) with a big-endian register pointer.
 *
 * This driver does **not** ship the full per-resolution register
 * tables.  Those are large (~700 lines per preset, vendor-supplied)
 * and live in v0.3 once the alp_camera capture path on V2N exercises
 * them.  v0.2 verifies the chip ID, exposes the `set_*` API surface,
 * and toggles the documented bring-up bits (system reset,
 * test-pattern output).  Calling set_resolution / set_format on this
 * driver returns ALP_OK if the preset is in range, but the chip is
 * not actually reconfigured — see TODO blocks below.
 */

#include <stddef.h>

#include "alp/chips/ov5640.h"

/* ------------------------------------------------------------------ */
/* Register addresses (datasheet §8)                                   */
/* ------------------------------------------------------------------ */

#define REG_SYS_CTRL0      0x3008   /* bit7 = software reset, bit6 = power down. */
#define REG_PRE_PATTERN    0x503D   /* test-pattern enable. */

static alp_status_t reg_write8(ov5640_t *dev, uint16_t reg, uint8_t val) {
    uint8_t buf[3] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xFFu),
        val,
    };
    return alp_i2c_write(dev->bus, dev->addr, buf, sizeof buf);
}

static alp_status_t reg_read8(ov5640_t *dev, uint16_t reg, uint8_t *out) {
    uint8_t addr_be[2] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xFFu),
    };
    return alp_i2c_write_read(dev->bus, dev->addr,
                              addr_be, sizeof addr_be,
                              out, 1);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

alp_status_t ov5640_init(ov5640_t *dev, alp_i2c_t *bus, uint8_t i2c_addr) {
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (i2c_addr == 0) return ALP_ERR_INVAL;

    dev->bus  = bus;
    dev->addr = i2c_addr;
    dev->res  = OV5640_RES_VGA;
    dev->fmt  = OV5640_FMT_RGB565;
    dev->initialised = false;

    uint16_t id = 0;
    alp_status_t s = ov5640_read_id(dev, &id);
    if (s != ALP_OK) return s;
    if (id != OV5640_CHIP_ID) return ALP_ERR_IO;

    dev->initialised = true;
    return ALP_OK;
}

alp_status_t ov5640_read_id(ov5640_t *dev, uint16_t *id_out) {
    if (dev == NULL || dev->bus == NULL || id_out == NULL) return ALP_ERR_INVAL;
    uint8_t hi = 0, lo = 0;
    alp_status_t s = reg_read8(dev, OV5640_REG_CHIP_ID_HI, &hi);
    if (s != ALP_OK) return s;
    s = reg_read8(dev, OV5640_REG_CHIP_ID_LO, &lo);
    if (s != ALP_OK) return s;
    *id_out = (uint16_t)(((uint16_t)hi << 8) | lo);
    return ALP_OK;
}

alp_status_t ov5640_soft_reset(ov5640_t *dev) {
    if (dev == NULL || dev->bus == NULL) return ALP_ERR_INVAL;
    /* SYS_CTRL0 bit7 = software reset.  Caller waits ≥1 ms before
     * touching any other register per datasheet §8.3. */
    return reg_write8(dev, REG_SYS_CTRL0, 0x82u);
}

alp_status_t ov5640_set_resolution(ov5640_t *dev, ov5640_resolution_t res) {
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (res > OV5640_RES_5MP) return ALP_ERR_INVAL;
    /* The resolution-specific register table from OmniVision's reference
     * init script lands in v0.3 alongside the alp_camera_v2n capture
     * path.  Until then this returns NOSUPPORT explicitly -- silent OK
     * would be a contract lie (the caller would believe the sensor was
     * reconfigured when it's actually still at the 2592×1944 default).
     * The chosen preset is remembered in `dev->res` so v0.3 can apply
     * it without re-asking the caller. */
    dev->res = res;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t ov5640_set_format(ov5640_t *dev, ov5640_format_t fmt) {
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (fmt > OV5640_FMT_RAW8) return ALP_ERR_INVAL;
    /* FORMAT_CTRL (0x4300) + FORMAT_CTRL_MUX (0x501F) writes land in
     * v0.3.  Same rationale as ov5640_set_resolution -- the chosen
     * preset is remembered in `dev->fmt`, but the driver explicitly
     * declines the write rather than masquerading as a silent OK. */
    dev->fmt = fmt;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t ov5640_set_test_pattern(ov5640_t *dev, bool enabled) {
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    /* Pre-ISP test-pattern output (colour bar) — register 0x503D bit7. */
    return reg_write8(dev, REG_PRE_PATTERN, (uint8_t)(enabled ? 0x80u : 0x00u));
}

void ov5640_deinit(ov5640_t *dev) {
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus = NULL;
}
