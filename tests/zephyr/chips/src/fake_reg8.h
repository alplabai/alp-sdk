/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared 8-bit register-file core for the chip-driver i2c-emul
 * fakes.  Models the canonical register-pointer protocol:
 *   write  [reg, d0, d1, ...]  -- set pointer, store bytes (auto-inc)
 *   write  [reg] + read N      -- set pointer, read N bytes (auto-inc)
 * A per-chip write hook overrides plain stores (e.g. RWC1 event
 * registers).  Every data-byte write is appended to a small log so
 * tests can assert WRITE ORDER, not just final state.
 * Hooks that need to flag a protocol violation should call
 * ztest_fail() directly -- fake_reg8_transfer always reports transfer
 * success.
 */
#ifndef ALP_TESTS_FAKE_REG8_H
#define ALP_TESTS_FAKE_REG8_H

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>

/* Every EMUL_DT_INST_DEFINE child of the i2c-emul controller needs a
 * paired no-op device: the controller's emuls_<N> array references
 * the child's __device_dts_ord_<N> symbol, which only a
 * DEVICE_DT_INST_DEFINE emits (persists under Zephyr v4.4.0 -- not a
 * 3.7 artifact).  Invoke once per fake translation unit, after the
 * EMUL_DT_INST_DEFINE foreach. */
#define FAKE_EMUL_DEV_SHIM() DT_INST_FOREACH_STATUS_OKAY(FAKE_EMUL_DEV_SHIM_ONE)
#define FAKE_EMUL_DEV_SHIM_ONE(n)                                                                  \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                  \
                          CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);

#define FAKE_REG8_LOG_DEPTH 32u

struct fake_reg8_wr {
    uint8_t reg;
    uint8_t val;
};

struct fake_reg8 {
    uint8_t regs[256];
    uint8_t ptr;
    void (*write_hook)(struct fake_reg8 *f, uint8_t reg, uint8_t val);
    struct fake_reg8_wr log[FAKE_REG8_LOG_DEPTH];
    uint8_t             log_count; /* saturates at FAKE_REG8_LOG_DEPTH */
};

static inline void fake_reg8_reset(struct fake_reg8 *f)
{
    void (*hook)(struct fake_reg8 *, uint8_t, uint8_t) = f->write_hook;
    memset(f, 0, sizeof(*f));
    f->write_hook = hook;
}

static inline void fake_reg8_store(struct fake_reg8 *f, uint8_t reg, uint8_t val)
{
    if (f->log_count < FAKE_REG8_LOG_DEPTH) {
        f->log[f->log_count].reg = reg;
        f->log[f->log_count].val = val;
        f->log_count++;
    }
    if (f->write_hook != NULL) {
        f->write_hook(f, reg, val);
    } else {
        f->regs[reg] = val;
    }
}

static inline int fake_reg8_transfer(struct fake_reg8 *f, struct i2c_msg *msgs, int num_msgs)
{
    for (int i = 0; i < num_msgs; i++) {
        struct i2c_msg *m = &msgs[i];
        if ((m->flags & I2C_MSG_READ) != 0u) {
            /* auto-increment read from the current pointer */
            for (uint32_t j = 0; j < m->len; j++) {
                m->buf[j] = f->regs[f->ptr];
                f->ptr++; /* wraps at 0xFF, like the real pointer */
            }
        } else {
            if (m->len == 0u) return -EIO; /* malformed: a write must carry the register pointer */
            /* first byte of a write sets the register pointer */
            f->ptr = m->buf[0];
            for (uint32_t j = 1; j < m->len; j++) {
                fake_reg8_store(f, f->ptr, m->buf[j]);
                f->ptr++; /* wraps at 0xFF, like the real pointer */
            }
        }
    }
    return 0;
}

#endif /* ALP_TESTS_FAKE_REG8_H */
