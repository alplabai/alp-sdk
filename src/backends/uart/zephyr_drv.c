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
#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "alp_errno.h"
#include "uart_ops.h"

#define ALP_UART_DEV_OR_NULL(idx) \
	COND_CODE_1(DT_NODE_HAS_STATUS(DT_ALIAS(_CONCAT(alp_uart, idx)), okay), \
	            (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_uart, idx)))), \
	            (NULL))

static const struct device *const _devs[] = {
	ALP_UART_DEV_OR_NULL(0), ALP_UART_DEV_OR_NULL(1), ALP_UART_DEV_OR_NULL(2),
	ALP_UART_DEV_OR_NULL(3), ALP_UART_DEV_OR_NULL(4), ALP_UART_DEV_OR_NULL(5),
	ALP_UART_DEV_OR_NULL(6), ALP_UART_DEV_OR_NULL(7),
};

static enum uart_config_parity _to_zephyr_parity(alp_uart_parity_t p)
{
	switch (p) {
	case ALP_UART_PARITY_EVEN:
		return UART_CFG_PARITY_EVEN;
	case ALP_UART_PARITY_ODD:
		return UART_CFG_PARITY_ODD;
	default:
		return UART_CFG_PARITY_NONE;
	}
}

static enum uart_config_data_bits _to_zephyr_data_bits(uint8_t bits)
{
	switch (bits) {
	case 5:
		return UART_CFG_DATA_BITS_5;
	case 6:
		return UART_CFG_DATA_BITS_6;
	case 7:
		return UART_CFG_DATA_BITS_7;
	case 9:
		return UART_CFG_DATA_BITS_9;
	case 8:
	default:
		return UART_CFG_DATA_BITS_8;
	}
}

static enum uart_config_stop_bits _to_zephyr_stop_bits(uint8_t bits)
{
	return (bits == 2) ? UART_CFG_STOP_BITS_2 : UART_CFG_STOP_BITS_1;
}

/* Maps alp_uart_flow_t -> Zephyr's uart_config.flow_ctrl.  Returns false for
 * ALP_UART_FLOW_XON_XOFF: Zephyr's enum uart_config_flow_control carries only
 * NONE / RTS_CTS / DTR_DSR / RS485, no in-band-software equivalent -- the
 * caller (z_open) turns a false return into ALP_ERR_NOSUPPORT rather than
 * silently opening with flow control off (issue #1639). */
static bool _to_zephyr_flow_ctrl(alp_uart_flow_t flow, enum uart_config_flow_control *out)
{
	switch (flow) {
	case ALP_UART_FLOW_NONE:
		*out = UART_CFG_FLOW_CTRL_NONE;
		return true;
	case ALP_UART_FLOW_RTS_CTS:
		*out = UART_CFG_FLOW_CTRL_RTS_CTS;
		return true;
	default:
		return false;
	}
}

static alp_status_t _errno_to_alp(int err)
{
	/* Delegates to the shared negative-errno baseline (issue #1638).
	 * BEHAVIOUR CHANGE: this switch had no -EAGAIN and/or no -ETIMEDOUT
	 * arm, so a driver-reported deadline surfaced as ALP_ERR_IO.  Callers
	 * can now receive ALP_ERR_TIMEOUT here, and ALP_ERR_NOT_READY /
	 * ALP_ERR_NOMEM / ALP_ERR_NOSUPPORT for the other arms the switch
	 * lacked.  Every arm it DID carry agreed with the baseline. */
	return alp_status_from_zephyr_errno(err);
}

static alp_status_t
z_open(const alp_uart_config_t *cfg, alp_uart_backend_state_t *st, alp_capabilities_t *caps_out)
{
	if (cfg->port_id >= ARRAY_SIZE(_devs)) return ALP_ERR_INVAL;
	if (cfg->port_id >= ALP_SOC_UART_COUNT) return ALP_ERR_OUT_OF_RANGE;
	const struct device *dev = _devs[cfg->port_id];
	if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;

	enum uart_config_flow_control flow_ctrl;
	if (!_to_zephyr_flow_ctrl(cfg->flow_control, &flow_ctrl)) {
		/* ALP_UART_FLOW_XON_XOFF has no Zephyr equivalent -- refuse
		 * up front instead of configuring NONE and returning ALP_OK
		 * for a request the driver never even saw (issue #1639). */
		return ALP_ERR_NOSUPPORT;
	}

	struct uart_config zcfg = {
		.baudrate  = cfg->baudrate,
		.parity    = _to_zephyr_parity(cfg->parity),
		.stop_bits = _to_zephyr_stop_bits(cfg->stop_bits),
		.data_bits = _to_zephyr_data_bits(cfg->data_bits),
		.flow_ctrl = flow_ctrl,
	};
	int err = uart_configure(dev, &zcfg);
	if (err == -ENOSYS || err == -ENOTSUP) {
		/* Some controllers / shims don't expose runtime configuration --
         * accept ENOSYS / ENOTSUP and trust the devicetree-provided params,
         * UNLESS the caller asked for flow control: trusting the
         * devicetree there would let a controller that can't honour
         * RTS/CTS accept the field and drop it, reporting ALP_OK for a
         * link that silently has no flow control (issue #1639). */
		if (cfg->flow_control != ALP_UART_FLOW_NONE) {
			return ALP_ERR_NOSUPPORT;
		}
	} else if (err != 0) {
		return _errno_to_alp(err);
	} else if (cfg->flow_control != ALP_UART_FLOW_NONE) {
		/* uart_configure() reporting success does not mean the
		 * driver acted on flow_ctrl -- nothing in the Zephyr UART
		 * API requires a driver to reject a value it merely stores
		 * without applying it to real hardware.  uart_emul is NOT
		 * such a driver: its own .config_get mirrors back whatever
		 * .configure stored (drivers/serial/uart_emul.c), so this
		 * readback can never catch it -- see this suite's
		 * test_uart_open_rts_cts_reaches_the_driver_on_uart1, which
		 * asserts the opposite (an honoured request must NOT be
		 * false-rejected here).  The guard exists for a controller
		 * whose .config_get reports real hardware state independent
		 * of what was merely requested (issue #1639). */
		struct uart_config back;
		int                gerr = uart_config_get(dev, &back);
		if (gerr == 0 && back.flow_ctrl != flow_ctrl) {
			return ALP_ERR_NOSUPPORT;
		}
		/* gerr != 0: the driver can't report its current config --
		 * nothing left to check locally, so trust uart_configure()'s
		 * ALP_OK the same way the ENOSYS/ENOTSUP branch above trusts
		 * the devicetree-provided params. */
	}

	st->dev         = (void *)dev;
	st->port_id     = cfg->port_id;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t z_write(alp_uart_backend_state_t *st, const uint8_t *data, size_t len)
{
	const struct device *dev = (const struct device *)st->dev;
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(dev, data[i]);
	}
	return ALP_OK;
}

static alp_status_t
z_read(alp_uart_backend_state_t *st, uint8_t *data, size_t len, uint32_t timeout_ms)
{
	const struct device *dev = (const struct device *)st->dev;
	/* One absolute deadline for the WHOLE call (not one budget per
     * byte), so timeout_ms bounds inter-byte gaps too.  timeout_ms == 0
     * yields deadline == now: the "deadline already reached" check
     * below is what turns that into a single non-blocking poll,
     * matching the portable contract -- see alp_uart_read() in
     * <alp/peripheral.h> and its Yocto twin in src/yocto/peripheral_uart.c
     * (#595/#621).  The old (timeout_ms == 0) ? INT64_MAX : ... form
     * made a zero timeout mean "wait forever" instead. */
	const int64_t deadline = k_uptime_get() + (int64_t)timeout_ms;

	size_t got = 0;
	while (got < len) {
		int err = uart_poll_in(dev, &data[got]);
		if (err == 0) {
			got++;
			continue;
		}
		if (err != -1) {
			/* Genuine driver error -- map and report directly. */
			return _errno_to_alp(err);
		}

		/* err == -1: no byte ready yet.  Deadline expiry with at
         * least one byte already collected is a partial read, per
         * the documented contract -- only an empty read times out. */
		if (k_uptime_get() >= deadline) {
			return (got > 0) ? ALP_OK : ALP_ERR_TIMEOUT;
		}
		/* k_msleep(1) instead of k_yield() so the system
         * tick actually advances on native_sim (k_yield
         * with no other ready thread is a no-op there,
         * making the timeout deadline unreachable). */
		k_msleep(1);
	}
	return ALP_OK;
}

/* Detach any RX ring buffer still attached when the parent UART handle
 * closes -- see alp_uart_rx_ringbuf_attach()/_detach() below.  Declared
 * here (ahead of its definition) so it can sit in the ops table next to
 * open/write/read; defined after the ringbuf bodies further down. */
static void z_close(alp_uart_backend_state_t *st);

static const alp_uart_ops_t _ops = {
	.open  = z_open,
	.write = z_write,
	.read  = z_read,
	.close = z_close,
};

ALP_BACKEND_REGISTER(uart,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

/* Runs on every alp_uart_close(), whether or not a ring buffer was ever
 * attached.  alp_uart_rx_ringbuf_detach() is defined further down (real
 * teardown when CONFIG_ALP_SDK_UART_RX_RINGBUF=y, a no-op stub otherwise),
 * so this stays a single definition for both configs -- the prototype is
 * already visible via <alp/peripheral.h> included above. */
static void z_close(alp_uart_backend_state_t *st)
{
	if (st->rx_ringbuf != NULL) {
		alp_uart_rx_ringbuf_detach((alp_uart_rx_ringbuf_t *)st->rx_ringbuf);
		st->rx_ringbuf = NULL;
	}
}

/* ================================================================== */
/* RX ring buffer (CONFIG_ALP_SDK_UART_RX_RINGBUF)                     */
/* ================================================================== */

#include "alp_z_last_error.h"

#if defined(CONFIG_ALP_SDK_UART_RX_RINGBUF)

#include "alp_slot_claim.h"
#include "../../zephyr/handles.h"

/* Test-only synchronisation hook (default no-op; a single NULL-check
 * branch in production) -- mirrors src/backends/rpc/zephyr_drv.c's
 * g_rpc_recv_test_sync_hook, but non-static: tests/zephyr/
 * uart_rx_ringbuf_close_race links the real built alp_sdk library
 * rather than #including this .c file directly, so the hook needs
 * external linkage to be reachable from that test TU. Lets the test
 * pause alp_uart_rx_ringbuf_pop() here -- right after
 * alp_handle_op_enter() has counted the op in, before the actual
 * lwrb_read() -- so it can drive alp_uart_rx_ringbuf_detach()'s drain
 * against a counted op made through the REAL public entry point.
 * Runs outside any lock, so it is safe for it to block. */
void (*alp_uart_rx_ringbuf_pop_test_sync_hook)(void) = NULL;

/* IRQ-context drain: pull bytes out of the controller FIFO into the
 * caller's LwRB.  Single-producer / single-consumer holds because
 * Zephyr serialises the UART IRQ callback against itself and the
 * consumer thread is the only reader (alp_uart_rx_ringbuf_pop).
 * Bytes that overflow the ring are dropped on the floor -- the ring
 * acts as a back-pressure indicator, not a guarantee of zero loss.
 * Callers that need lossless capture should size the backing store
 * to cover the worst-case drain latency. */
static void alp_uart_rx_isr(const struct device *dev, void *user_data)
{
	struct alp_uart_rx_ringbuf *s = user_data;
	if (s == NULL || !s->in_use) return;
	while (uart_irq_update(dev) > 0 && uart_irq_rx_ready(dev) > 0) {
		uint8_t scratch[32];
		int     n = uart_fifo_read(dev, scratch, sizeof(scratch));
		if (n <= 0) break;
		(void)lwrb_write(&s->rb, scratch, (size_t)n);
	}
}

alp_uart_rx_ringbuf_t *
alp_uart_rx_ringbuf_attach(alp_uart_t *port, uint8_t *backing, size_t backing_size)
{
	alp_z_clear_last_error();
	if (port == NULL || !port->in_use || backing == NULL || backing_size < 2u) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	/* IRQ-driven RX requires a real Zephyr device; the sw_fallback
     * backend leaves state.dev = NULL -- reject it cleanly. */
	const struct device *dev = (const struct device *)port->state.dev;
	if (dev == NULL) {
		alp_z_set_last_error(ALP_ERR_NOSUPPORT);
		return NULL;
	}
	/* Exclusive ownership: only one ring buffer may be attached to a
     * port at a time (Zephyr exposes a single IRQ callback slot per
     * UART device).  A second attach would silently steal the device
     * callback out from under the first handle -- reject it instead. */
	if (port->state.rx_ringbuf != NULL) {
		alp_z_set_last_error(ALP_ERR_BUSY);
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
	s->dev  = dev;
	s->port = port;

	int err = uart_irq_callback_user_data_set(dev, alp_uart_rx_isr, s);
	if (err != 0) {
		alp_z_uart_rx_ringbuf_pool_release(s);
		alp_z_set_last_error(_errno_to_alp(err));
		return NULL;
	}
	uart_irq_rx_enable(dev);
	port->state.rx_ringbuf = s;
	/* Publish OPEN only after every field above is populated, so a
	 * concurrent pop/count/detach racing this attach sees a fully-
	 * initialised handle the instant it observes LC_OPEN. Issue #1634. */
	alp_lifecycle_set(&s->lifecycle, ALP_HANDLE_LC_OPEN);
	return s;
}

alp_status_t
alp_uart_rx_ringbuf_pop(alp_uart_rx_ringbuf_t *rb, uint8_t *out, size_t max_len, size_t *got)
{
	if (got != NULL) *got = 0;
	if (rb == NULL) return ALP_ERR_NOT_READY;
	/* Count this op in before touching the ring: a racing detach()
	 * that has already begun cannot recycle the slot until this op
	 * leaves. Issue #1634 -- was a bare `!rb->in_use` read, so a
	 * detach() mid-pop could free/reassign the same struct address
	 * (lwrb_free + a fresh attach()'s lwrb_init) while this call was
	 * still reading out of it, delivering another owner's bytes. */
	if (!alp_handle_op_enter(&rb->lifecycle, &rb->active_ops)) return ALP_ERR_NOT_READY;
	if (alp_uart_rx_ringbuf_pop_test_sync_hook != NULL) {
		alp_uart_rx_ringbuf_pop_test_sync_hook();
	}

	alp_status_t rc = ALP_OK;
	if (out == NULL && max_len > 0) {
		rc = ALP_ERR_INVAL;
	} else if (max_len > 0) {
		size_t n = lwrb_read(&rb->rb, out, max_len);
		if (got != NULL) *got = n;
	}
	alp_handle_op_leave(&rb->active_ops);
	return rc;
}

size_t alp_uart_rx_ringbuf_count(const alp_uart_rx_ringbuf_t *rb)
{
	if (rb == NULL) return 0;
	/* rb is logically const to the caller, but lifecycle/active_ops are
	 * atomic bookkeeping mutated even through a read-only handle (same
	 * rationale as a mutex embedded in a const struct) -- mirrors
	 * pop()'s guard so count() can't race a concurrent detach() either.
	 * Issue #1634. */
	struct alp_uart_rx_ringbuf *mrb = (struct alp_uart_rx_ringbuf *)rb;
	if (!alp_handle_op_enter(&mrb->lifecycle, &mrb->active_ops)) return 0;
	size_t n = lwrb_get_full(&mrb->rb);
	alp_handle_op_leave(&mrb->active_ops);
	return n;
}

void alp_uart_rx_ringbuf_detach(alp_uart_rx_ringbuf_t *rb)
{
	if (rb == NULL) return;
	/* begin_close CAS OPEN->CLOSING then sleep-polls until every op that
	 * entered before the CAS has left -- alp_uart_rx_ringbuf_pop() runs
	 * on the consumer thread only (see <alp/peripheral.h>'s documented
	 * contract) but nothing restricts detach()/close() to that thread,
	 * so the drain still matters. Idempotent: a second/never-attached
	 * detach no-ops. Issue #1634. */
	if (!alp_handle_begin_close_blocking(&rb->lifecycle, &rb->active_ops)) return;
	if (rb->dev != NULL) {
		uart_irq_rx_disable(rb->dev);
		(void)uart_irq_callback_user_data_set(rb->dev, NULL, NULL);
	}
	/* Clear the parent port's back-ref so a fresh attach is accepted
     * afterwards -- but only if it still points at this handle.  The
     * UART handle pool is a static array: once a port closes, its slot
     * can be handed to an unrelated alp_uart_open() before this detach
     * runs, and we must not clobber that new owner's live back-ref. */
	if (rb->port != NULL && rb->port->state.rx_ringbuf == rb) {
		rb->port->state.rx_ringbuf = NULL;
	}
	lwrb_free(&rb->rb);
	alp_lifecycle_set(&rb->lifecycle, ALP_HANDLE_LC_UNOPENED);
	alp_z_uart_rx_ringbuf_pool_release(rb);
}

#else /* !CONFIG_ALP_SDK_UART_RX_RINGBUF */

alp_uart_rx_ringbuf_t *
alp_uart_rx_ringbuf_attach(alp_uart_t *port, uint8_t *backing, size_t backing_size)
{
	(void)port;
	(void)backing;
	(void)backing_size;
	alp_z_clear_last_error();
	alp_z_set_last_error(ALP_ERR_NOSUPPORT);
	return NULL;
}

alp_status_t
alp_uart_rx_ringbuf_pop(alp_uart_rx_ringbuf_t *rb, uint8_t *out, size_t max_len, size_t *got)
{
	(void)rb;
	(void)out;
	(void)max_len;
	if (got != NULL) *got = 0;
	return ALP_ERR_NOSUPPORT;
}

size_t alp_uart_rx_ringbuf_count(const alp_uart_rx_ringbuf_t *rb)
{
	(void)rb;
	return 0;
}

void alp_uart_rx_ringbuf_detach(alp_uart_rx_ringbuf_t *rb)
{
	(void)rb;
}

#endif /* CONFIG_ALP_SDK_UART_RX_RINGBUF */
