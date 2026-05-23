/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr i2s_* driver-class backend.  Used on any SoC
 * unless a vendor-specific backend registers a more specific
 * silicon_ref match.  Pooling lives in src/i2s_dispatch.c; the
 * backend's open resolves the alp-i2sN DT alias, allocates the
 * 2-block ping-pong slab + its backing buffer in a per-handle
 * Zephyr sidecar, and configures the i2s controller.
 *
 * The portable handle in src/backends/i2s/i2s_ops.h carries no
 * Zephyr types -- k_mem_slab is a Zephyr-typed object, so it
 * cannot live inside struct alp_i2s without contaminating the
 * portable surface.  The sidecar (alp_z_i2s_side_t) holds the
 * slab + slab_buf; state->be_data carries the per-handle pointer.
 * The sidecar pool is sized by CONFIG_ALP_SDK_MAX_I2S_HANDLES so
 * every active dispatcher slot has a matching sidecar.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/i2s.h>
#include <alp/soc_caps.h>

#include "i2s_ops.h"

#define ALP_I2S_DEV_OR_NULL(idx) \
    COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_i2s, idx))), \
                (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_i2s, idx)))), (NULL))

static const struct device *const _devs[] = {
    ALP_I2S_DEV_OR_NULL(0),
    ALP_I2S_DEV_OR_NULL(1),
};

#ifndef CONFIG_ALP_SDK_MAX_I2S_HANDLES
#define CONFIG_ALP_SDK_MAX_I2S_HANDLES 2
#endif

/* Per-handle Zephyr sidecar.  Holds the k_mem_slab + its heap-backed
 * buffer so the portable handle in src/backends/i2s/i2s_ops.h stays
 * free of <zephyr/kernel.h> types.  state->be_data carries the
 * per-handle pointer set at open() time. */
typedef struct {
    struct k_mem_slab  mem_slab;
    uint8_t           *slab_buf;
    size_t             slab_buf_bytes;
    bool               in_use;
} alp_z_i2s_side_t;

static alp_z_i2s_side_t _sides[CONFIG_ALP_SDK_MAX_I2S_HANDLES];

static alp_z_i2s_side_t *_alloc_side(void) {
    for (size_t i = 0; i < ARRAY_SIZE(_sides); ++i) {
        if (!_sides[i].in_use) {
            _sides[i] = (alp_z_i2s_side_t){0};
            _sides[i].in_use = true;
            return &_sides[i];
        }
    }
    return NULL;
}

static void _free_side(alp_z_i2s_side_t *s) {
    if (s != NULL) s->in_use = false;
}

static alp_status_t _errno_to_alp(int err) {
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

static enum i2s_dir _to_dir(alp_i2s_dir_t d) {
    switch (d) {
    case ALP_I2S_DIR_RX:   return I2S_DIR_RX;
    case ALP_I2S_DIR_TX:   return I2S_DIR_TX;
    case ALP_I2S_DIR_BOTH: return I2S_DIR_BOTH;
    default:               return I2S_DIR_RX;
    }
}

static i2s_fmt_t _to_fmt(alp_i2s_format_t f) {
    switch (f) {
    case ALP_I2S_FMT_I2S:             return I2S_FMT_DATA_FORMAT_I2S;
    case ALP_I2S_FMT_LEFT_JUSTIFIED:  return I2S_FMT_DATA_FORMAT_LEFT_JUSTIFIED;
    case ALP_I2S_FMT_RIGHT_JUSTIFIED: return I2S_FMT_DATA_FORMAT_RIGHT_JUSTIFIED;
    case ALP_I2S_FMT_PCM_SHORT:       return I2S_FMT_DATA_FORMAT_PCM_SHORT;
    case ALP_I2S_FMT_PCM_LONG:        return I2S_FMT_DATA_FORMAT_PCM_LONG;
    default:                          return I2S_FMT_DATA_FORMAT_I2S;
    }
}

static alp_status_t z_open(const alp_i2s_config_t *cfg,
                           alp_i2s_backend_state_t *st,
                           alp_capabilities_t *caps_out) {
    if (cfg->bus_id >= ARRAY_SIZE(_devs)) return ALP_ERR_INVAL;
    if (cfg->bus_id >= ALP_SOC_I2S_COUNT) return ALP_ERR_OUT_OF_RANGE;
    const struct device *dev = _devs[cfg->bus_id];
    if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;

    alp_z_i2s_side_t *s = _alloc_side();
    if (s == NULL) return ALP_ERR_NOMEM;

    /* Allocate a 2-block ping-pong slab.  Memory comes from the
     * caller's heap via k_malloc; on M-class targets the application
     * enables CONFIG_HEAP_MEM_POOL_SIZE.  Block bytes = frames ×
     * channels × (word_bits / 8). */
    size_t block_bytes = (size_t)cfg->block_frames *
                         (size_t)cfg->channels *
                         (size_t)((cfg->word_bits + 7u) / 8u);
    s->slab_buf_bytes = block_bytes * 2u;
    s->slab_buf = k_malloc(s->slab_buf_bytes);
    if (s->slab_buf == NULL) {
        _free_side(s);
        return ALP_ERR_NOMEM;
    }
    int err = k_mem_slab_init(&s->mem_slab, s->slab_buf, block_bytes, 2);
    if (err != 0) {
        k_free(s->slab_buf);
        _free_side(s);
        return _errno_to_alp(err);
    }

    struct i2s_config zcfg = {
        .word_size      = cfg->word_bits,
        .channels       = cfg->channels,
        .format         = _to_fmt(cfg->format),
        .options        = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER,
        .frame_clk_freq = cfg->sample_rate_hz,
        .mem_slab       = &s->mem_slab,
        .block_size     = block_bytes,
        .timeout        = SYS_FOREVER_MS,
    };
    err = i2s_configure(dev, _to_dir(cfg->direction), &zcfg);
    if (err != 0) {
        k_free(s->slab_buf);
        _free_side(s);
        return _errno_to_alp(err);
    }

    st->dev     = (void *)dev;
    st->bus_id  = cfg->bus_id;
    st->be_data = s;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t z_start(alp_i2s_backend_state_t *st) {
    struct alp_i2s *h = CONTAINER_OF(st, struct alp_i2s, state);
    const struct device *dev = (const struct device *)st->dev;
    return _errno_to_alp(i2s_trigger(dev, _to_dir(h->cfg.direction),
                                     I2S_TRIGGER_START));
}

static alp_status_t z_stop(alp_i2s_backend_state_t *st) {
    struct alp_i2s *h = CONTAINER_OF(st, struct alp_i2s, state);
    const struct device *dev = (const struct device *)st->dev;
    return _errno_to_alp(i2s_trigger(dev, _to_dir(h->cfg.direction),
                                     I2S_TRIGGER_DRAIN));
}

static alp_status_t z_write(alp_i2s_backend_state_t *st,
                            const void *block, size_t bytes,
                            uint32_t timeout_ms) {
    alp_z_i2s_side_t *s = (alp_z_i2s_side_t *)st->be_data;
    const struct device *dev = (const struct device *)st->dev;
    if (s == NULL || dev == NULL) return ALP_ERR_NOT_READY;

    void *slab_block = NULL;
    int err = k_mem_slab_alloc(&s->mem_slab, &slab_block, K_MSEC(timeout_ms));
    if (err != 0 || slab_block == NULL) {
        return _errno_to_alp(err ? err : -ETIMEDOUT);
    }
    memcpy(slab_block, block, bytes);
    err = i2s_write(dev, slab_block, bytes);
    if (err != 0) {
        k_mem_slab_free(&s->mem_slab, slab_block);
        return _errno_to_alp(err);
    }
    return ALP_OK;
}

static alp_status_t z_read(alp_i2s_backend_state_t *st,
                           void *block, size_t bytes,
                           size_t *bytes_out,
                           uint32_t timeout_ms) {
    (void)timeout_ms;     /* upstream i2s_read has no per-call timeout */
    alp_z_i2s_side_t *s = (alp_z_i2s_side_t *)st->be_data;
    const struct device *dev = (const struct device *)st->dev;
    if (s == NULL || dev == NULL) return ALP_ERR_NOT_READY;

    void *slab_block = NULL;
    size_t got = 0u;
    int err = i2s_read(dev, &slab_block, &got);
    if (err != 0) return _errno_to_alp(err);

    if (got > bytes) got = bytes;
    memcpy(block, slab_block, got);
    k_mem_slab_free(&s->mem_slab, slab_block);
    if (bytes_out != NULL) *bytes_out = got;
    return ALP_OK;
}

static void z_close(alp_i2s_backend_state_t *st) {
    alp_z_i2s_side_t *s = (alp_z_i2s_side_t *)st->be_data;
    const struct device *dev = (const struct device *)st->dev;
    struct alp_i2s *h = CONTAINER_OF(st, struct alp_i2s, state);

    /* If the dispatcher latched started=true without a matching stop,
     * drop the in-flight frames before releasing the slab so the
     * controller doesn't reference freed memory. */
    if (h->started && dev != NULL) {
        (void)i2s_trigger(dev, _to_dir(h->cfg.direction), I2S_TRIGGER_DROP);
    }
    if (s != NULL) {
        if (s->slab_buf != NULL) {
            k_free(s->slab_buf);
            s->slab_buf = NULL;
        }
        _free_side(s);
    }
    st->be_data = NULL;
}

static const alp_i2s_ops_t _ops = {
    .open  = z_open,
    .start = z_start,
    .stop  = z_stop,
    .write = z_write,
    .read  = z_read,
    .close = z_close,
};

ALP_BACKEND_REGISTER(i2s, zephyr_drv, {
    .silicon_ref = "*",
    .vendor      = "zephyr",
    .base_caps   = 0u,
    .priority    = 100,
    .ops         = &_ops,
    .probe       = NULL,
});
