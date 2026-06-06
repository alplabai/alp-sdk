/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake SSD1306 i2c-emul target.  Records each I2C write into separate
 * command and data byte logs, split by the SSD1306 control byte
 * (0x00 = command stream, 0x40 = data stream).  The chips ztest
 * uses the logs to verify the driver's init opcode sequence and
 * the address-window framing on `ssd1306_display`.
 */

#define DT_DRV_COMPAT alp_fake_ssd1306

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fake_reg8.h"
#include "fakes.h"

#define LOG_CAP 4096    /* Enough for full-screen 128×64 push (1024 B) + init. */

struct fake_ssd1306_data {
    uint8_t  cmd_log [LOG_CAP];
    size_t   cmd_len;
    uint8_t  data_log[LOG_CAP];
    size_t   data_len;
};

static struct fake_ssd1306_data *g_fake_ssd1306;

static void log_append(uint8_t *buf, size_t *len, size_t cap,
                       const uint8_t *src, size_t n) {
    size_t free = cap - *len;
    if (n > free) n = free;
    memcpy(buf + *len, src, n);
    *len += n;
}

static int fake_ssd1306_transfer(const struct emul *target,
                                 struct i2c_msg *msgs, int num_msgs,
                                 int addr) {
    (void)addr;
    struct fake_ssd1306_data *d = target->data;

    for (int m = 0; m < num_msgs; m++) {
        if ((msgs[m].flags & I2C_MSG_READ) != 0) {
            /* SSD1306 driver only writes — anything else is a test bug. */
            return -EIO;
        }
        if (msgs[m].len < 1) continue;
        const uint8_t ctrl = msgs[m].buf[0];
        if (ctrl == 0x00u) {
            log_append(d->cmd_log, &d->cmd_len, sizeof d->cmd_log,
                       msgs[m].buf + 1, msgs[m].len - 1);
        } else if (ctrl == 0x40u) {
            log_append(d->data_log, &d->data_len, sizeof d->data_log,
                       msgs[m].buf + 1, msgs[m].len - 1);
        } else {
            /* Unknown control byte. */
            return -EIO;
        }
    }
    return 0;
}

static const struct i2c_emul_api fake_ssd1306_api = {
    .transfer = fake_ssd1306_transfer,
};

static int fake_ssd1306_init(const struct emul *target,
                             const struct device *parent) {
    (void)parent;
    struct fake_ssd1306_data *d = target->data;
    g_fake_ssd1306 = d;
    d->cmd_len  = 0;
    d->data_len = 0;
    return 0;
}

#define FAKE_SSD1306_DEFINE(n)                                           \
    static struct fake_ssd1306_data fake_ssd1306_data_##n;               \
    EMUL_DT_INST_DEFINE(n, fake_ssd1306_init,                            \
                        &fake_ssd1306_data_##n, NULL,                    \
                        &fake_ssd1306_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(FAKE_SSD1306_DEFINE)

FAKE_EMUL_DEV_SHIM()

/* ------------------------------------------------------------------ */
/* Test-side inspection API                                             */
/* ------------------------------------------------------------------ */

size_t fake_ssd1306_cmd_log_len(void) {
    return g_fake_ssd1306 ? g_fake_ssd1306->cmd_len : 0u;
}

const uint8_t *fake_ssd1306_cmd_log(void) {
    return g_fake_ssd1306 ? g_fake_ssd1306->cmd_log : NULL;
}

size_t fake_ssd1306_data_log_len(void) {
    return g_fake_ssd1306 ? g_fake_ssd1306->data_len : 0u;
}

const uint8_t *fake_ssd1306_data_log(void) {
    return g_fake_ssd1306 ? g_fake_ssd1306->data_log : NULL;
}

void fake_ssd1306_reset_logs(void) {
    if (!g_fake_ssd1306) return;
    g_fake_ssd1306->cmd_len  = 0;
    g_fake_ssd1306->data_len = 0;
}
