/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/i2s.h>.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/sys/util.h>

#include "alp/i2s.h"
#include "alp/soc_caps.h"
#include "handles.h"

#define ALP_I2S_DEV_OR_NULL(idx)                                               \
    COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_i2s, idx))),               \
                (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_i2s, idx)))), (NULL))

static const struct device *const alp_i2s_devs[] = {
    ALP_I2S_DEV_OR_NULL(0),
    ALP_I2S_DEV_OR_NULL(1),
};

static alp_status_t errno_to_alp(int err) {
    switch (err) {
    case 0:           return ALP_OK;
    case -EINVAL:     return ALP_ERR_INVAL;
    case -EBUSY:      return ALP_ERR_BUSY;
    case -EAGAIN:
    case -ETIMEDOUT:  return ALP_ERR_TIMEOUT;
    case -EIO:        return ALP_ERR_IO;
    case -ENOTSUP:
    case -ENOSYS:     return ALP_ERR_NOSUPPORT;
    default:          return ALP_ERR_IO;
    }
}

static enum i2s_dir to_dir(alp_i2s_dir_t d) {
    switch (d) {
    case ALP_I2S_DIR_RX:   return I2S_DIR_RX;
    case ALP_I2S_DIR_TX:   return I2S_DIR_TX;
    case ALP_I2S_DIR_BOTH: return I2S_DIR_BOTH;
    default:               return I2S_DIR_RX;
    }
}

static i2s_fmt_t to_fmt(alp_i2s_format_t f) {
    switch (f) {
    case ALP_I2S_FMT_I2S:             return I2S_FMT_DATA_FORMAT_I2S;
    case ALP_I2S_FMT_LEFT_JUSTIFIED:  return I2S_FMT_DATA_FORMAT_LEFT_JUSTIFIED;
    case ALP_I2S_FMT_RIGHT_JUSTIFIED: return I2S_FMT_DATA_FORMAT_RIGHT_JUSTIFIED;
    case ALP_I2S_FMT_PCM_SHORT:       return I2S_FMT_DATA_FORMAT_PCM_SHORT;
    case ALP_I2S_FMT_PCM_LONG:        return I2S_FMT_DATA_FORMAT_PCM_LONG;
    default:                          return I2S_FMT_DATA_FORMAT_I2S;
    }
}

alp_i2s_t *alp_i2s_open(const alp_i2s_config_t *cfg) {
    alp_z_clear_last_error();

    if (cfg == NULL || cfg->channels == 0 || cfg->channels > 2 || cfg->block_frames == 0) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (cfg->word_bits != 8 && cfg->word_bits != 16 && cfg->word_bits != 24 &&
        cfg->word_bits != 32) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (cfg->bus_id >= ARRAY_SIZE(alp_i2s_devs)) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (cfg->bus_id >= ALP_SOC_I2S_COUNT) {
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }

    const struct device *dev = alp_i2s_devs[cfg->bus_id];
    if (dev == NULL || !device_is_ready(dev)) {
        alp_z_set_last_error(ALP_ERR_NOT_READY);
        return NULL;
    }

    struct alp_i2s *h = alp_z_i2s_pool_acquire();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    h->bus_id  = cfg->bus_id;
    h->dev     = dev;
    h->cfg     = *cfg;
    h->started = false;

    /* Allocate a 2-block ping-pong slab.  Memory comes from the
     * caller's heap via k_malloc; on M-class targets the
     * application enables CONFIG_HEAP_MEM_POOL_SIZE.  Block bytes
     * = frames × channels × (word_bits/8). */
    size_t block_bytes = (size_t)cfg->block_frames *
                         (size_t)cfg->channels *
                         (size_t)((cfg->word_bits + 7u) / 8u);
    h->slab_buf_bytes = block_bytes * 2;
    h->slab_buf = k_malloc(h->slab_buf_bytes);
    if (h->slab_buf == NULL) {
        alp_z_i2s_pool_release(h);
        return NULL;
    }
    int err = k_mem_slab_init(&h->mem_slab, h->slab_buf, block_bytes, 2);
    if (err != 0) {
        k_free(h->slab_buf);
        alp_z_i2s_pool_release(h);
        return NULL;
    }

    struct i2s_config zcfg = {
        .word_size      = cfg->word_bits,
        .channels       = cfg->channels,
        .format         = to_fmt(cfg->format),
        .options        = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER,
        .frame_clk_freq = cfg->sample_rate_hz,
        .mem_slab       = &h->mem_slab,
        .block_size     = block_bytes,
        .timeout        = SYS_FOREVER_MS,
    };
    err = i2s_configure(dev, to_dir(cfg->direction), &zcfg);
    if (err != 0) {
        k_free(h->slab_buf);
        alp_z_i2s_pool_release(h);
        return NULL;
    }
    return h;
}

alp_status_t alp_i2s_start(alp_i2s_t *i2s) {
    if (i2s == NULL || !i2s->in_use) return ALP_ERR_NOT_READY;
    int err = i2s_trigger(i2s->dev, to_dir(i2s->cfg.direction), I2S_TRIGGER_START);
    if (err == 0) i2s->started = true;
    return errno_to_alp(err);
}

alp_status_t alp_i2s_stop(alp_i2s_t *i2s) {
    if (i2s == NULL || !i2s->in_use) return ALP_ERR_NOT_READY;
    int err = i2s_trigger(i2s->dev, to_dir(i2s->cfg.direction), I2S_TRIGGER_DRAIN);
    if (err == 0) i2s->started = false;
    return errno_to_alp(err);
}

alp_status_t alp_i2s_write(alp_i2s_t *i2s,
                           const void *block, size_t bytes,
                           uint32_t timeout_ms) {
    if (i2s == NULL || !i2s->in_use) return ALP_ERR_NOT_READY;
    if (block == NULL || bytes == 0) return ALP_ERR_INVAL;

    void *slab_block = NULL;
    int err = k_mem_slab_alloc(&i2s->mem_slab, &slab_block, K_MSEC(timeout_ms));
    if (err != 0 || slab_block == NULL) return errno_to_alp(err ? err : -ETIMEDOUT);
    memcpy(slab_block, block, bytes);
    err = i2s_write(i2s->dev, slab_block, bytes);
    if (err != 0) {
        k_mem_slab_free(&i2s->mem_slab, slab_block);
        return errno_to_alp(err);
    }
    return ALP_OK;
}

alp_status_t alp_i2s_read(alp_i2s_t *i2s,
                          void *block, size_t bytes,
                          size_t *bytes_out,
                          uint32_t timeout_ms) {
    (void)timeout_ms;
    if (i2s == NULL || !i2s->in_use) return ALP_ERR_NOT_READY;
    if (block == NULL || bytes == 0) return ALP_ERR_INVAL;
    if (bytes_out != NULL) *bytes_out = 0;

    void *slab_block = NULL;
    size_t got = 0;
    int err = i2s_read(i2s->dev, &slab_block, &got);
    if (err != 0) return errno_to_alp(err);

    if (got > bytes) got = bytes;
    memcpy(block, slab_block, got);
    k_mem_slab_free(&i2s->mem_slab, slab_block);
    if (bytes_out != NULL) *bytes_out = got;
    return ALP_OK;
}

void alp_i2s_close(alp_i2s_t *i2s) {
    if (i2s == NULL || !i2s->in_use) return;
    if (i2s->started) {
        (void)i2s_trigger(i2s->dev, to_dir(i2s->cfg.direction),
                          I2S_TRIGGER_DROP);
    }
    if (i2s->slab_buf != NULL) {
        k_free(i2s->slab_buf);
        i2s->slab_buf = NULL;
    }
    alp_z_i2s_pool_release(i2s);
}
