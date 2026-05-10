/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/mproc.h> -- multi-processor IPC.
 *
 * Replaces the v0.1 NOSUPPORT stub.  Wraps Zephyr's MBOX driver
 * (Alif's MHU controller is registered as a Zephyr mbox device on
 * AEN), the hwsem driver class, and a DT-anchored shared-memory
 * region for the M55-HP <-> M55-HE peer pair.
 *
 * The DT alias `alp-shmemN` points at a memory-region node; v0.3
 * supports name = "alp_shmem0" mapping to alias 0.  The mbox
 * channel comes from the studio-supplied alp_mbox_config.channel
 * (resolved through the alp-mboxN alias).  The hwsem from
 * alp-hwsemN.
 *
 * Gated on CONFIG_ALP_SDK_MPROC.  When OFF, NULL/NOSUPPORT.
 */

#include <errno.h>
#include <string.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "alp/mproc.h"
#include "handles.h"

#if defined(CONFIG_ALP_SDK_MPROC)
#include <zephyr/device.h>
#include <zephyr/drivers/mbox.h>
#endif

#ifndef CONFIG_ALP_SDK_MAX_SHMEM_HANDLES
#define CONFIG_ALP_SDK_MAX_SHMEM_HANDLES 2
#endif
#ifndef CONFIG_ALP_SDK_MAX_MBOX_HANDLES
#define CONFIG_ALP_SDK_MAX_MBOX_HANDLES 4
#endif
#ifndef CONFIG_ALP_SDK_MAX_HWSEM_HANDLES
#define CONFIG_ALP_SDK_MAX_HWSEM_HANDLES 4
#endif

/* ------------------------------------------------------------------ */
/* Internal handle structures                                          */
/* ------------------------------------------------------------------ */

struct alp_shmem {
    bool in_use;
#if defined(CONFIG_ALP_SDK_MPROC)
    void  *base;
    size_t size;
#endif
};

struct alp_mbox {
    bool in_use;
#if defined(CONFIG_ALP_SDK_MPROC)
    const struct device *dev;
    uint32_t             channel;
    alp_mbox_msg_cb_t    cb;
    void                *cb_user;
    struct mbox_channel  tx_chan;
    struct mbox_channel  rx_chan;
#endif
};

struct alp_hwsem {
    bool in_use;
#if defined(CONFIG_ALP_SDK_MPROC)
    uint32_t hwsem_id;
    bool     held;
#endif
};

#if defined(CONFIG_ALP_SDK_MPROC)
static struct alp_shmem  g_shmem_pool[CONFIG_ALP_SDK_MAX_SHMEM_HANDLES];
static struct alp_mbox   g_mbox_pool[CONFIG_ALP_SDK_MAX_MBOX_HANDLES];
static struct alp_hwsem  g_hwsem_pool[CONFIG_ALP_SDK_MAX_HWSEM_HANDLES];

static struct alp_shmem *shmem_pool_acquire(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_shmem_pool); ++i) {
        if (!g_shmem_pool[i].in_use) {
            memset(&g_shmem_pool[i], 0, sizeof(g_shmem_pool[i]));
            g_shmem_pool[i].in_use = true;
            return &g_shmem_pool[i];
        }
    }
    return NULL;
}

static struct alp_mbox *mbox_pool_acquire(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_mbox_pool); ++i) {
        if (!g_mbox_pool[i].in_use) {
            memset(&g_mbox_pool[i], 0, sizeof(g_mbox_pool[i]));
            g_mbox_pool[i].in_use = true;
            return &g_mbox_pool[i];
        }
    }
    return NULL;
}

static struct alp_hwsem *hwsem_pool_acquire(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_hwsem_pool); ++i) {
        if (!g_hwsem_pool[i].in_use) {
            memset(&g_hwsem_pool[i], 0, sizeof(g_hwsem_pool[i]));
            g_hwsem_pool[i].in_use = true;
            return &g_hwsem_pool[i];
        }
    }
    return NULL;
}

static alp_status_t errno_to_alp(int err)
{
    switch (err) {
    case 0:
        return ALP_OK;
    case -EINVAL:
        return ALP_ERR_INVAL;
    case -EBUSY:
        return ALP_ERR_BUSY;
    case -EAGAIN:
    case -ETIMEDOUT:
        return ALP_ERR_TIMEOUT;
    case -EIO:
        return ALP_ERR_IO;
    case -ENOTSUP:
    case -ENOSYS:
        return ALP_ERR_NOSUPPORT;
    case -ENOMEM:
        return ALP_ERR_NOMEM;
    default:
        return ALP_ERR_IO;
    }
}
#endif /* CONFIG_ALP_SDK_MPROC */

/* ================================================================== */
/* Shared memory                                                       */
/* ================================================================== */

alp_shmem_t *alp_shmem_open(const alp_shmem_config_t *cfg)
{
    alp_z_clear_last_error();
    if (cfg == NULL || cfg->name == NULL) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
#if defined(CONFIG_ALP_SDK_MPROC)
    /* DT-anchored shared regions land in v0.3.x once the M55-HE
     * peer firmware build pipeline is in place.  The wrapper shape
     * stands; the runtime mapping returns NOSUPPORT until the
     * memory-region nodes appear in the EVK overlay. */
    (void)cfg;
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
#else
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
#endif
}

alp_status_t alp_shmem_view(alp_shmem_t *s, void **base_out, size_t *size_out)
{
    if (base_out != NULL) *base_out = NULL;
    if (size_out != NULL) *size_out = 0;
    if (s == NULL || !s->in_use) return ALP_ERR_NOT_READY;
#if defined(CONFIG_ALP_SDK_MPROC)
    if (base_out != NULL) *base_out = s->base;
    if (size_out != NULL) *size_out = s->size;
    return ALP_OK;
#else
    return ALP_ERR_NOSUPPORT;
#endif
}

void alp_shmem_close(alp_shmem_t *s)
{
    if (s == NULL || !s->in_use) return;
#if defined(CONFIG_ALP_SDK_MPROC)
    s->in_use = false;
#endif
}

/* ================================================================== */
/* Mailbox                                                             */
/* ================================================================== */

#if defined(CONFIG_ALP_SDK_MPROC)

#define ALP_MBOX_DEV_OR_NULL(idx)                                                                  \
    COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_mbox, idx))),                                  \
                (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_mbox, idx)))), (NULL))

static const struct device *const alp_mbox_devs[] = {
    ALP_MBOX_DEV_OR_NULL(0),
    ALP_MBOX_DEV_OR_NULL(1),
    ALP_MBOX_DEV_OR_NULL(2),
    ALP_MBOX_DEV_OR_NULL(3),
};

static void mbox_rx_cb(const struct device *dev, mbox_channel_id_t channel_id, void *user_data,
                       struct mbox_msg *data)
{
    (void)dev;
    struct alp_mbox *mb = (struct alp_mbox *)user_data;
    if (mb == NULL || mb->cb == NULL) return;
    mb->cb((uint32_t)channel_id, data ? data->data : NULL, data ? data->size : 0, mb->cb_user);
}

#endif /* CONFIG_ALP_SDK_MPROC */

alp_mbox_t *alp_mbox_open(const alp_mbox_config_t *cfg)
{
    alp_z_clear_last_error();
    if (cfg == NULL) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
#if defined(CONFIG_ALP_SDK_MPROC)
    if (cfg->channel >= ARRAY_SIZE(alp_mbox_devs)) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    const struct device *dev = alp_mbox_devs[cfg->channel];
    if (dev == NULL || !device_is_ready(dev)) {
        alp_z_set_last_error(ALP_ERR_NOT_READY);
        return NULL;
    }
    struct alp_mbox *mb = mbox_pool_acquire();
    if (mb == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    mb->dev     = dev;
    mb->channel = cfg->channel;
    mbox_init_channel(&mb->tx_chan, dev, cfg->channel);
    mbox_init_channel(&mb->rx_chan, dev, cfg->channel);
    return mb;
#else
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
#endif
}

alp_status_t alp_mbox_send(alp_mbox_t *mb, const void *data, size_t len, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (mb == NULL || !mb->in_use) return ALP_ERR_NOT_READY;
    if (data == NULL && len > 0) return ALP_ERR_INVAL;
#if defined(CONFIG_ALP_SDK_MPROC)
    struct mbox_msg msg = {
        .data = data,
        .size = len,
    };
    return errno_to_alp(mbox_send(&mb->tx_chan, &msg));
#else
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_mbox_set_callback(alp_mbox_t *mb, alp_mbox_msg_cb_t cb, void *user)
{
    if (mb == NULL || !mb->in_use) return ALP_ERR_NOT_READY;
#if defined(CONFIG_ALP_SDK_MPROC)
    mb->cb      = cb;
    mb->cb_user = user;
    int err     = mbox_register_callback(&mb->rx_chan, cb ? mbox_rx_cb : NULL, mb);
    if (err == 0) {
        err = mbox_set_enabled(&mb->rx_chan, cb ? true : false);
    }
    return errno_to_alp(err);
#else
    (void)cb;
    (void)user;
    return ALP_ERR_NOSUPPORT;
#endif
}

void alp_mbox_close(alp_mbox_t *mb)
{
    if (mb == NULL || !mb->in_use) return;
#if defined(CONFIG_ALP_SDK_MPROC)
    (void)mbox_set_enabled(&mb->rx_chan, false);
    mb->in_use = false;
#endif
}

/* ================================================================== */
/* Hardware semaphore                                                  */
/* ================================================================== */

/* Zephyr 3.7 doesn't ship a fully-portable hwsem driver class --
 * support is per-vendor (Alif provides hwsem in their HAL, ST has
 * a separate driver, etc.).  v0.3.x lands the per-SoC hwsem hooks
 * once the AEN HIL CI runs against a real Alif HWSEM register
 * map.  For v0.3 the wrapper shape is in place and validates
 * args; the real lock/unlock falls through to NOSUPPORT. */

alp_hwsem_t *alp_hwsem_open(uint32_t hwsem_id)
{
    alp_z_clear_last_error();
#if defined(CONFIG_ALP_SDK_MPROC)
    if (hwsem_id >= 16) {
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }
    struct alp_hwsem *s = hwsem_pool_acquire();
    if (s == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    s->hwsem_id = hwsem_id;
    s->held     = false;
    return s;
#else
    (void)hwsem_id;
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
#endif
}

alp_status_t alp_hwsem_try_lock(alp_hwsem_t *sem)
{
    if (sem == NULL || !sem->in_use) return ALP_ERR_NOT_READY;
    /* SoC-specific HWSEM register access lands v0.3.x. */
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_hwsem_lock(alp_hwsem_t *sem, uint32_t timeout_ms)
{
    if (sem == NULL || !sem->in_use) return ALP_ERR_NOT_READY;
    (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_hwsem_unlock(alp_hwsem_t *sem)
{
    if (sem == NULL || !sem->in_use) return ALP_ERR_NOT_READY;
    return ALP_ERR_NOSUPPORT;
}

void alp_hwsem_close(alp_hwsem_t *sem)
{
    if (sem == NULL || !sem->in_use) return;
#if defined(CONFIG_ALP_SDK_MPROC)
    sem->in_use = false;
#endif
}
