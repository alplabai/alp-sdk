/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software I2C fallback.  Deterministic in-memory loopback for
 * native_sim builds; not a real bus.
 *
 * write(addr, data, len)  -- stashes the last frame in a static
 *   buffer keyed by addr; the buffer holds up to SW_BUF_LEN bytes.
 * read(addr, data, len)   -- returns the stashed frame for addr,
 *   zero-padded if shorter, truncated if longer.
 * write_read              -- write phase followed immediately by
 *   a read from the same addr (loopback echo for test coverage).
 *
 * Priority 0, silicon_ref="*": always loses to zephyr_drv
 * (priority 100) on real silicon; picked only when the test build
 * forces it via CONFIG_ALP_SDK_I2C_SW_FALLBACK=y with no Zephyr
 * I2C devices present.
 *
 * @par Cost: ROM ~600 B, RAM 2 * SW_BUF_LEN bytes (two static
 *      frame buffers -- last_addr + last_data + last_len).
 * @par Performance: O(len) per write/read (memcpy); deterministic
 *      for test assertions.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "i2c_ops.h"

#define SW_BUF_LEN  64u

static uint8_t  _buf[SW_BUF_LEN];
static size_t   _buf_len  = 0u;
static uint8_t  _buf_addr = 0u;

static alp_status_t sw_open(const alp_i2c_config_t *cfg,
                            alp_i2c_backend_state_t *st,
                            alp_capabilities_t *caps_out) {
    (void)cfg;
    st->dev    = NULL;
    st->bus_id = 0u;
    st->be_data = NULL;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t sw_write(alp_i2c_backend_state_t *st,
                             uint8_t addr,
                             const uint8_t *data, size_t len) {
    (void)st;
    _buf_addr = addr;
    _buf_len  = (len < SW_BUF_LEN) ? len : SW_BUF_LEN;
    if (_buf_len > 0u && data != NULL) {
        memcpy(_buf, data, _buf_len);
    }
    return ALP_OK;
}

static alp_status_t sw_read(alp_i2c_backend_state_t *st,
                            uint8_t addr,
                            uint8_t *data, size_t len) {
    (void)st;
    if (len == 0u) return ALP_OK;
    if (addr != _buf_addr) {
        /* Unknown address -- return all zeros */
        memset(data, 0, len);
        return ALP_OK;
    }
    size_t copy = (_buf_len < len) ? _buf_len : len;
    if (copy > 0u) memcpy(data, _buf, copy);
    if (copy < len) memset(data + copy, 0, len - copy);
    return ALP_OK;
}

static alp_status_t sw_write_read(alp_i2c_backend_state_t *st,
                                  uint8_t addr,
                                  const uint8_t *wdata, size_t wlen,
                                  uint8_t *rdata, size_t rlen) {
    alp_status_t rc = sw_write(st, addr, wdata, wlen);
    if (rc != ALP_OK) return rc;
    return sw_read(st, addr, rdata, rlen);
}

static const alp_i2c_ops_t _ops = {
    .open       = sw_open,
    .write      = sw_write,
    .read       = sw_read,
    .write_read = sw_write_read,
    .close      = NULL,
};

ALP_BACKEND_REGISTER(i2c, sw_fallback, {
    .silicon_ref = "*",
    .vendor      = "sw",
    .base_caps   = 0u,
    .priority    = 0,
    .ops         = &_ops,
    .probe       = NULL,
});
