/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr uart_* driver-class backend.  Used on any SoC
 * unless a vendor-specific backend registers a more specific
 * silicon_ref match.  Pooling lives in src/uart_dispatch.c; the
 * backend's open fills state->dev and configures the UART parameters.
 *
 * The alp_uart_rx_ringbuf_* bodies are appended below, guarded by
 * CONFIG_ALP_SDK_UART_RX_RINGBUF.  They are Zephyr driver-class-
 * specific (uart_irq_callback_set / uart_irq_rx_enable) and do not
 * enter the ops vtable; the dispatcher's alp_uart_open path has no
 * knowledge of them.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "uart_ops.h"

#define ALP_UART_DEV_OR_NULL(idx) \
    COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_uart, idx))), \
                (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_uart, idx)))), (NULL))

static const struct device *const _devs[] = {
    ALP_UART_DEV_OR_NULL(0),
    ALP_UART_DEV_OR_NULL(1),
    ALP_UART_DEV_OR_NULL(2),
    ALP_UART_DEV_OR_NULL(3),
    ALP_UART_DEV_OR_NULL(4),
    ALP_UART_DEV_OR_NULL(5),
    ALP_UART_DEV_OR_NULL(6),
    ALP_UART_DEV_OR_NULL(7),
};

static enum uart_config_parity _to_zephyr_parity(alp_uart_parity_t p) {
    switch (p) {
    case ALP_UART_PARITY_EVEN: return UART_CFG_PARITY_EVEN;
    case ALP_UART_PARITY_ODD:  return UART_CFG_PARITY_ODD;
    default:                   return UART_CFG_PARITY_NONE;
    }
}

static enum uart_config_data_bits _to_zephyr_data_bits(uint8_t bits) {
    switch (bits) {
    case 5:  return UART_CFG_DATA_BITS_5;
    case 6:  return UART_CFG_DATA_BITS_6;
    case 7:  return UART_CFG_DATA_BITS_7;
    case 9:  return UART_CFG_DATA_BITS_9;
    case 8:
    default: return UART_CFG_DATA_BITS_8;
    }
}

static enum uart_config_stop_bits _to_zephyr_stop_bits(uint8_t bits) {
    return (bits == 2) ? UART_CFG_STOP_BITS_2 : UART_CFG_STOP_BITS_1;
}

static alp_status_t _errno_to_alp(int err) {
    switch (err) {
    case 0:           return ALP_OK;
    case -EINVAL:     return ALP_ERR_INVAL;
    case -EBUSY:      return ALP_ERR_BUSY;
    case -ETIMEDOUT:  return ALP_ERR_TIMEOUT;
    case -EIO:        return ALP_ERR_IO;
    case -ENOTSUP:
    case -ENOSYS:     return ALP_ERR_NOSUPPORT;
    default:          return ALP_ERR_IO;
    }
}

static alp_status_t z_open(const alp_uart_config_t *cfg,
                           alp_uart_backend_state_t *st,
                           alp_capabilities_t *caps_out) {
    if (cfg->port_id >= ARRAY_SIZE(_devs)) return ALP_ERR_INVAL;
    if (cfg->port_id >= ALP_SOC_UART_COUNT) return ALP_ERR_OUT_OF_RANGE;
    const struct device *dev = _devs[cfg->port_id];
    if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;

    struct uart_config zcfg = {
        .baudrate  = cfg->baudrate,
        .parity    = _to_zephyr_parity(cfg->parity),
        .stop_bits = _to_zephyr_stop_bits(cfg->stop_bits),
        .data_bits = _to_zephyr_data_bits(cfg->data_bits),
        .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
    };
    int err = uart_configure(dev, &zcfg);
    /* Some controllers / shims don't expose runtime configuration --
     * accept ENOSYS / ENOTSUP and trust the devicetree-provided params. */
    if (err != 0 && err != -ENOSYS && err != -ENOTSUP) {
        return _errno_to_alp(err);
    }

    st->dev     = dev;
    st->port_id = cfg->port_id;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t z_write(alp_uart_backend_state_t *st,
                            const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(st->dev, data[i]);
    }
    return ALP_OK;
}

static alp_status_t z_read(alp_uart_backend_state_t *st,
                           uint8_t *data, size_t len,
                           uint32_t timeout_ms) {
    const int64_t deadline = (timeout_ms == 0)
        ? INT64_MAX
        : k_uptime_get() + (int64_t)timeout_ms;

    for (size_t i = 0; i < len; i++) {
        int err;
        do {
            err = uart_poll_in(st->dev, &data[i]);
            if (err == -1 && k_uptime_get() >= deadline) {
                return ALP_ERR_TIMEOUT;
            }
            if (err == -1) {
                /* k_msleep(1) instead of k_yield() so the system
                 * tick actually advances on native_sim (k_yield
                 * with no other ready thread is a no-op there,
                 * making the timeout deadline unreachable). */
                k_msleep(1);
            }
        } while (err == -1);

        if (err != 0) return _errno_to_alp(err);
    }
    return ALP_OK;
}

static const alp_uart_ops_t _ops = {
    .open  = z_open,
    .write = z_write,
    .read  = z_read,
    .close = NULL,     /* no teardown needed for uart_configure */
};

ALP_BACKEND_REGISTER(uart, zephyr_drv, {
    .silicon_ref = "*",
    .vendor      = "zephyr",
    .base_caps   = 0u,
    .priority    = 100,
    .ops         = &_ops,
    .probe       = NULL,
});

/* ================================================================== */
/* RX ring buffer (CONFIG_ALP_SDK_UART_RX_RINGBUF)                     */
/* ================================================================== */

extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#if defined(CONFIG_ALP_SDK_UART_RX_RINGBUF)

#include <lwrb/lwrb.h>

/* Forward-declare the pool functions from handles.c so we can call
 * them without pulling in handles.h (which would re-define struct alp_uart
 * -- already defined via uart_ops.h above). */
struct alp_uart_rx_ringbuf;
extern struct alp_uart_rx_ringbuf *alp_z_uart_rx_ringbuf_pool_acquire(void);
extern void alp_z_uart_rx_ringbuf_pool_release(struct alp_uart_rx_ringbuf *h);

/* The full struct is needed inside this TU to read/write fields.
 * Replicate the definition here using the same layout as handles.h;
 * the two definitions are identical and both guarded by the same
 * CONFIG flag so the compiler sees only one per TU. */
struct alp_uart_rx_ringbuf {
    bool                 in_use;
    const struct device *dev;     /* mirror of port->state.dev for ISR use */
    struct alp_uart     *port;    /* back-ref for detach */
    lwrb_t               rb;
};

/* IRQ-context drain: pull bytes out of the controller FIFO into the
 * caller's LwRB.  Single-producer / single-consumer holds because
 * Zephyr serialises the UART IRQ callback against itself and the
 * consumer thread is the only reader (alp_uart_rx_ringbuf_pop).
 * Bytes that overflow the ring are dropped on the floor -- the ring
 * acts as a back-pressure indicator, not a guarantee of zero loss.
 * Callers that need lossless capture should size the backing store
 * to cover the worst-case drain latency. */
static void alp_uart_rx_isr(const struct device *dev, void *user_data) {
    struct alp_uart_rx_ringbuf *s = user_data;
    if (s == NULL || !s->in_use) return;
    while (uart_irq_update(dev) > 0 && uart_irq_rx_ready(dev) > 0) {
        uint8_t scratch[32];
        int n = uart_fifo_read(dev, scratch, sizeof(scratch));
        if (n <= 0) break;
        (void)lwrb_write(&s->rb, scratch, (size_t)n);
    }
}

alp_uart_rx_ringbuf_t *alp_uart_rx_ringbuf_attach(alp_uart_t *port,
                                                  uint8_t    *backing,
                                                  size_t      backing_size) {
    alp_z_clear_last_error();
    if (port == NULL || !port->in_use || backing == NULL || backing_size < 2u) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    struct alp_uart_rx_ringbuf *s = alp_z_uart_rx_ringbuf_pool_acquire();
    if (s == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    if (lwrb_init(&s->rb, backing, backing_size) == 0u) {
        alp_z_uart_rx_ringbuf_pool_release(s);
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    s->dev  = port->state.dev;
    s->port = port;

    int err = uart_irq_callback_user_data_set(port->state.dev, alp_uart_rx_isr, s);
    if (err != 0) {
        alp_z_uart_rx_ringbuf_pool_release(s);
        alp_z_set_last_error(_errno_to_alp(err));
        return NULL;
    }
    uart_irq_rx_enable(port->state.dev);
    return s;
}

alp_status_t alp_uart_rx_ringbuf_pop(alp_uart_rx_ringbuf_t *rb,
                                     uint8_t *out, size_t max_len,
                                     size_t *got) {
    if (got != NULL) *got = 0;
    if (rb == NULL || !rb->in_use) return ALP_ERR_NOT_READY;
    if (out == NULL && max_len > 0) return ALP_ERR_INVAL;
    if (max_len == 0) return ALP_OK;
    size_t n = lwrb_read(&rb->rb, out, max_len);
    if (got != NULL) *got = n;
    return ALP_OK;
}

size_t alp_uart_rx_ringbuf_count(const alp_uart_rx_ringbuf_t *rb) {
    if (rb == NULL || !rb->in_use) return 0;
    return lwrb_get_full(&rb->rb);
}

void alp_uart_rx_ringbuf_detach(alp_uart_rx_ringbuf_t *rb) {
    if (rb == NULL || !rb->in_use) return;
    if (rb->dev != NULL) {
        uart_irq_rx_disable(rb->dev);
        (void)uart_irq_callback_user_data_set(rb->dev, NULL, NULL);
    }
    lwrb_free(&rb->rb);
    alp_z_uart_rx_ringbuf_pool_release(rb);
}

#else /* !CONFIG_ALP_SDK_UART_RX_RINGBUF */

alp_uart_rx_ringbuf_t *alp_uart_rx_ringbuf_attach(alp_uart_t *port,
                                                  uint8_t    *backing,
                                                  size_t      backing_size) {
    (void)port; (void)backing; (void)backing_size;
    alp_z_clear_last_error();
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
}

alp_status_t alp_uart_rx_ringbuf_pop(alp_uart_rx_ringbuf_t *rb,
                                     uint8_t *out, size_t max_len,
                                     size_t *got) {
    (void)rb; (void)out; (void)max_len;
    if (got != NULL) *got = 0;
    return ALP_ERR_NOSUPPORT;
}

size_t alp_uart_rx_ringbuf_count(const alp_uart_rx_ringbuf_t *rb) {
    (void)rb;
    return 0;
}

void alp_uart_rx_ringbuf_detach(alp_uart_rx_ringbuf_t *rb) {
    (void)rb;
}

#endif /* CONFIG_ALP_SDK_UART_RX_RINGBUF */
