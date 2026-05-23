/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C class dispatcher.  Routes the public alp_i2c_* API
 * through the .alp_backends_i2c registry.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "backends/i2c/i2c_ops.h"

ALP_BACKEND_DEFINE_CLASS(i2c);

extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#ifndef CONFIG_ALP_SDK_MAX_I2C_HANDLES
#define CONFIG_ALP_SDK_MAX_I2C_HANDLES 4
#endif

static struct alp_i2c _pool[CONFIG_ALP_SDK_MAX_I2C_HANDLES];

static struct alp_i2c *_alloc(void) {
    for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_I2C_HANDLES; ++i) {
        if (!_pool[i].in_use) {
            memset(&_pool[i], 0, sizeof(_pool[i]));
            _pool[i].in_use = true;
            return &_pool[i];
        }
    }
    return NULL;
}

static void _free(struct alp_i2c *h) { h->in_use = false; }

alp_i2c_t *alp_i2c_open(const alp_i2c_config_t *cfg) {
    alp_z_clear_last_error();
    if (cfg == NULL) { alp_z_set_last_error(ALP_ERR_INVAL); return NULL; }
    const alp_backend_t *be = alp_backend_select("i2c", ALP_SOC_REF_STR);
    if (be == NULL) { alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC); return NULL; }
    const alp_i2c_ops_t *ops = (const alp_i2c_ops_t *)be->ops;
    if (ops == NULL || ops->open == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED); return NULL;
    }
    struct alp_i2c *h = _alloc();
    if (h == NULL) { alp_z_set_last_error(ALP_ERR_NOMEM); return NULL; }
    h->backend = be;
    h->state.ops = ops;
    alp_capabilities_t caps = { .flags = be->base_caps };
    if (be->probe != NULL) {
        uint32_t refined = caps.flags;
        (void)be->probe(cfg->bus_id, &refined);
        caps.flags = refined;
    }
    alp_status_t rc = ops->open(cfg, &h->state, &caps);
    if (rc != ALP_OK) { _free(h); alp_z_set_last_error(rc); return NULL; }
    h->cached_caps = caps;
    return h;
}

alp_status_t alp_i2c_write(alp_i2c_t *bus, uint8_t addr,
                           const uint8_t *data, size_t len) {
    if (bus == NULL || !bus->in_use) return ALP_ERR_NOT_READY;
    if (data == NULL && len > 0) return ALP_ERR_INVAL;
    return bus->state.ops->write(&bus->state, addr, data, len);
}

alp_status_t alp_i2c_read(alp_i2c_t *bus, uint8_t addr,
                          uint8_t *data, size_t len) {
    if (bus == NULL || !bus->in_use) return ALP_ERR_NOT_READY;
    if (data == NULL && len > 0) return ALP_ERR_INVAL;
    return bus->state.ops->read(&bus->state, addr, data, len);
}

alp_status_t alp_i2c_write_read(alp_i2c_t *bus, uint8_t addr,
                                const uint8_t *wdata, size_t wlen,
                                uint8_t *rdata, size_t rlen) {
    if (bus == NULL || !bus->in_use) return ALP_ERR_NOT_READY;
    if ((wdata == NULL && wlen > 0) || (rdata == NULL && rlen > 0)) {
        return ALP_ERR_INVAL;
    }
    return bus->state.ops->write_read(&bus->state, addr, wdata, wlen, rdata, rlen);
}

void alp_i2c_close(alp_i2c_t *bus) {
    if (bus == NULL || !bus->in_use) return;
    if (bus->state.ops != NULL && bus->state.ops->close != NULL) {
        bus->state.ops->close(&bus->state);
    }
    _free(bus);
}

const alp_capabilities_t *alp_i2c_capabilities(const alp_i2c_t *bus) {
    return (bus != NULL) ? &bus->cached_caps : NULL;
}
