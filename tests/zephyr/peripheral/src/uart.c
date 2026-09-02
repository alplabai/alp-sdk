/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/peripheral.h> -- UART binding-layer smoke tests.  Extracted
 * from main.c in §C.16.  Includes the UART RX-ringbuf surface
 * (alp_uart_rx_ringbuf_*) since it sits behind the same backend.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/ztest.h>

#include "alp/peripheral.h"

/* alp-uart1 is a zephyr,uart-emul alias -- native_sim's overlays define
 * it (boards/native_sim.overlay, boards/native_sim_native_64.overlay)
 * but a real board has no emulated UART to alias, so any target
 * without the alias must still compile this file. */
#define HAVE_UART1_NODE DT_NODE_HAS_STATUS(DT_ALIAS(alp_uart1), okay)

/* alp-uart0 is mandatory on every target this suite builds for, but
 * WHICH driver backs it is not: native_sim's uart0 is
 * "zephyr,native-pty-uart" (drivers/serial/uart_native_pty.c), which
 * implements neither .configure nor .config_get -- forcing
 * uart_configure() to -ENOSYS regardless of what was asked for. A real
 * board's uart0 (e.g. the in-tree AEN overlay's alp-uart0 = &uart0, a
 * real Alif UART) implements both, so that -ENOSYS premise does not
 * generalise off native_sim. */
#define HAVE_NATIVE_PTY_UART0 DT_NODE_HAS_COMPAT(DT_ALIAS(alp_uart0), zephyr_native_pty_uart)

ZTEST(alp_peripheral, test_uart_open_close_roundtrip)
{
	alp_uart_t *u = alp_uart_open(&(alp_uart_config_t){
	    .port_id   = 0,
	    .baudrate  = 115200,
	    .data_bits = 8,
	    .stop_bits = 1,
	    .parity    = ALP_UART_PARITY_NONE,
	});
	zassert_not_null(u, "alp_uart_open should succeed for port_id=0");
	alp_uart_close(u);
}

ZTEST(alp_peripheral, test_uart_invalid_port_returns_null)
{
	alp_uart_t *u = alp_uart_open(&(alp_uart_config_t){ .port_id = 99 });
	zassert_is_null(u, "out-of-range port_id must yield NULL");
}

/* #1639: ALP_UART_FLOW_XON_XOFF has no Zephyr uart_config.flow_ctrl
 * equivalent (NONE / RTS_CTS / DTR_DSR / RS485 only) -- the backend
 * refuses it up front, before ever calling uart_configure(), so this
 * is deterministic on native_sim regardless of what the emulated
 * driver would otherwise accept. */
ZTEST(alp_peripheral, test_uart_open_rejects_xon_xoff_flow_control)
{
	alp_uart_t *u = alp_uart_open(&(alp_uart_config_t){
	    .port_id      = 0,
	    .baudrate     = 115200,
	    .data_bits    = 8,
	    .stop_bits    = 1,
	    .parity       = ALP_UART_PARITY_NONE,
	    .flow_control = ALP_UART_FLOW_XON_XOFF,
	});
	zassert_is_null(u, "XON/XOFF has no Zephyr flow_ctrl mapping");
	zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT, NULL);
}

/* #1639 (review follow-up): native_sim's uart0 (zephyr,native-pty-uart)
 * implements neither .configure nor .config_get, so uart_configure()
 * on port_id=0 always returns -ENOSYS -- deterministically forcing the
 * "controller can't attempt runtime configuration at all" arm of
 * z_open() without any fake ops table.  A caller that asked for real
 * flow control must still be refused here rather than silently getting
 * ALP_OK for devicetree-only params the driver never saw.  Gated on
 * HAVE_NATIVE_PTY_UART0 -- see its definition above -- because the
 * -ENOSYS premise is specific to that driver, not to port_id=0 in
 * general. */
#if HAVE_NATIVE_PTY_UART0
ZTEST(alp_peripheral, test_uart_open_rejects_rts_cts_when_controller_cannot_configure)
{
	alp_uart_t *u = alp_uart_open(&(alp_uart_config_t){
	    .port_id      = 0,
	    .baudrate     = 115200,
	    .data_bits    = 8,
	    .stop_bits    = 1,
	    .parity       = ALP_UART_PARITY_NONE,
	    .flow_control = ALP_UART_FLOW_RTS_CTS,
	});
	zassert_is_null(u, "uart0 cannot configure flow control at runtime");
	zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT, NULL);
}
#endif /* HAVE_NATIVE_PTY_UART0 */

/* ------------------------------------------------------------------ */
/* #626: alp_uart_read() timeout_ms contract.                          */
/*                                                                     */
/* timeout_ms == 0 is a single non-blocking poll; a positive value     */
/* bounds the ENTIRE call; a deadline reached with at least one byte   */
/* already collected is a partial read (ALP_OK), not a timeout -- see  */
/* alp_uart_read() in <alp/peripheral.h> and its Yocto twin in         */
/* src/yocto/peripheral_uart.c (#595/#621).                            */
/*                                                                     */
/* port_id 1 (alp-uart1) is a zephyr,uart-emul device, not native_sim's*/
/* real uart0 -- unlike uart0, its RX FIFO can be filled directly with */
/* uart_emul_put_rx_data(), which is what lets these tests drive the   */
/* read path deterministically without a live external UART peer.     */
/* ------------------------------------------------------------------ */

#if HAVE_UART1_NODE

static const struct device *const _uart1_emul_dev = DEVICE_DT_GET(DT_ALIAS(alp_uart1));

static alp_uart_t *_open_uart1(void)
{
	/* Every RX byte from a prior test must be gone before the next
     * one opens the port -- otherwise a leftover byte would turn a
     * "no data" case into a "data available" case. */
	uart_emul_flush_rx_data(_uart1_emul_dev);
	return alp_uart_open(&(alp_uart_config_t){
	    .port_id   = 1,
	    .baudrate  = 115200,
	    .data_bits = 8,
	    .stop_bits = 1,
	    .parity    = ALP_UART_PARITY_NONE,
	});
}

ZTEST(alp_peripheral, test_uart_read_zero_timeout_no_data_returns_immediately)
{
	alp_uart_t *u = _open_uart1();
	zassert_not_null(u, "alp_uart_open should succeed for port_id=1");

	uint8_t       byte    = 0xAAu;
	const int64_t before  = k_uptime_get();
	alp_status_t  s       = alp_uart_read(u, &byte, 1, 0);
	const int64_t elapsed = k_uptime_get() - before;

	zassert_equal(
	    s, ALP_ERR_TIMEOUT, "empty RX + timeout_ms=0 must be ALP_ERR_TIMEOUT, got %d", (int)s);
	/* Generous margin: this must be a single poll, not the old
     * accidental "block forever" (or even a short k_msleep(1) loop). */
	zassert_true(elapsed < 10, "timeout_ms=0 slept instead of polling once (%lld ms)", elapsed);

	alp_uart_close(u);
}

ZTEST(alp_peripheral, test_uart_read_zero_timeout_with_queued_byte_returns_ok)
{
	alp_uart_t *u = _open_uart1();
	zassert_not_null(u);

	uint8_t sent = 0x42u;
	zassert_equal(uart_emul_put_rx_data(_uart1_emul_dev, &sent, 1), 1u);

	uint8_t      got = 0;
	alp_status_t s   = alp_uart_read(u, &got, 1, 0);
	zassert_equal(s, ALP_OK, "queued byte + timeout_ms=0 must return ALP_OK, got %d", (int)s);
	zassert_equal(got, sent);

	alp_uart_close(u);
}

ZTEST(alp_peripheral, test_uart_read_finite_timeout_no_data_bounds_the_wait)
{
	alp_uart_t *u = _open_uart1();
	zassert_not_null(u);

	uint8_t       byte    = 0;
	const int64_t before  = k_uptime_get();
	alp_status_t  s       = alp_uart_read(u, &byte, 1, 50);
	const int64_t elapsed = k_uptime_get() - before;

	zassert_equal(
	    s, ALP_ERR_TIMEOUT, "no data before deadline must be ALP_ERR_TIMEOUT, got %d", (int)s);
	zassert_true(elapsed >= 50, "finite timeout returned early (%lld ms < 50)", elapsed);
	/* One absolute deadline for the WHOLE call -- must not run away
     * into a much longer wait (generous margin for scheduler jitter). */
	zassert_true(elapsed < 500, "finite timeout ran long past its deadline (%lld ms)", elapsed);

	alp_uart_close(u);
}

ZTEST(alp_peripheral, test_uart_read_data_available_returns_promptly)
{
	alp_uart_t *u = _open_uart1();
	zassert_not_null(u);

	uint8_t sent = 0x7Eu;
	zassert_equal(uart_emul_put_rx_data(_uart1_emul_dev, &sent, 1), 1u);

	uint8_t       got     = 0;
	const int64_t before  = k_uptime_get();
	alp_status_t  s       = alp_uart_read(u, &got, 1, 1000);
	const int64_t elapsed = k_uptime_get() - before;

	zassert_equal(s, ALP_OK, "got %d", (int)s);
	zassert_equal(got, sent);
	zassert_true(elapsed < 50,
	             "read with data already queued must not wait out the deadline (%lld ms)",
	             elapsed);

	alp_uart_close(u);
}

ZTEST(alp_peripheral, test_uart_read_partial_arrival_then_deadline_returns_ok)
{
	alp_uart_t *u = _open_uart1();
	zassert_not_null(u);

	/* Only the first of the two requested bytes ever arrives -- the
     * deadline expires with a partial read in flight, which the
     * documented contract folds to ALP_OK (not ALP_ERR_TIMEOUT). */
	uint8_t sent = 0x01u;
	zassert_equal(uart_emul_put_rx_data(_uart1_emul_dev, &sent, 1), 1u);

	uint8_t      buf[2] = { 0, 0 };
	alp_status_t s      = alp_uart_read(u, buf, sizeof(buf), 60);
	zassert_equal(s, ALP_OK, "partial read at deadline must be ALP_OK, got %d", (int)s);
	zassert_equal(buf[0], sent);

	alp_uart_close(u);
}

/* #1639 (review follow-up): uart_emul's .configure stores whatever it
 * is given and always returns 0 (drivers/serial/uart_emul.c) -- exactly
 * the "reports success but a driver could ignore the field" shape the
 * z_open() post-configure uart_config_get() readback exists to catch.
 * Because uart_emul's .config_get faithfully mirrors what was stored,
 * this asserts the OTHER half of that guard: a driver that DOES honour
 * RTS/CTS must not be false-rejected, and the value that reaches the
 * driver really is UART_CFG_FLOW_CTRL_RTS_CTS, not just whatever
 * alp_uart_open() returned. */
ZTEST(alp_peripheral, test_uart_open_rts_cts_reaches_the_driver_on_uart1)
{
	alp_uart_t *u = alp_uart_open(&(alp_uart_config_t){
	    .port_id      = 1,
	    .baudrate     = 115200,
	    .data_bits    = 8,
	    .stop_bits    = 1,
	    .parity       = ALP_UART_PARITY_NONE,
	    .flow_control = ALP_UART_FLOW_RTS_CTS,
	});
	zassert_not_null(u, "uart_emul honours flow_ctrl, open should succeed");

	struct uart_config back = { 0 };
	zassert_equal(uart_config_get(_uart1_emul_dev, &back), 0);
	zassert_equal(back.flow_ctrl,
	              UART_CFG_FLOW_CTRL_RTS_CTS,
	              "ALP_UART_FLOW_RTS_CTS must reach the driver as RTS_CTS");

	alp_uart_close(u);
}

#endif /* HAVE_UART1_NODE */

/* UART RX ringbuf: failure paths exercised on every build.  On builds
 * without CONFIG_ALP_SDK_UART_RX_RINGBUF the attach helper returns
 * NULL with ALP_ERR_NOSUPPORT regardless of arguments.  On builds with
 * the feature on, attach validates args + returns INVAL on NULL/zero
 * inputs.  Either way the binding compiles and the failure path is
 * deterministic.  Successful attach against a real device -- double
 * attach, detach, and close-cleanup -- is exercised further down under
 * native_sim's own uart0 (see the #600 section, CONFIG_ALP_SDK_UART_RX_RINGBUF
 * block); only true silicon RX byte delivery is parked behind an
 * explicitly-invoked bench run (docs/ci/HW-IN-LOOP.md). */
ZTEST(alp_peripheral, test_uart_rx_ringbuf_attach_null_port)
{
	uint8_t                buf[64];
	alp_uart_rx_ringbuf_t *rb = alp_uart_rx_ringbuf_attach(NULL, buf, sizeof(buf));
	zassert_is_null(rb, "NULL port must yield NULL");
	/* Either ALP_ERR_INVAL (feature on) or ALP_ERR_NOSUPPORT (feature off);
     * both are valid sentinels for a refused attach.  We just assert
     * "not OK" via the non-null absence of a handle above. */
}

ZTEST(alp_peripheral, test_uart_rx_ringbuf_pop_null_fails_safely)
{
	size_t       got = 99;
	uint8_t      dst[4];
	alp_status_t s = alp_uart_rx_ringbuf_pop(NULL, dst, sizeof(dst), &got);
	/* Exact code depends on whether the build has the feature on
     * (NOT_READY) or off (NOSUPPORT).  Either is a refusal -- the
     * contract is "does not write to out, zeroes got". */
	zassert_not_equal(s, ALP_OK, "NULL handle must not return ALP_OK");
	zassert_equal(got, 0u, "got must be zeroed on failure");
}

ZTEST(alp_peripheral, test_uart_rx_ringbuf_count_null_is_zero)
{
	zassert_equal(alp_uart_rx_ringbuf_count(NULL), 0u);
}

ZTEST(alp_peripheral, test_uart_rx_ringbuf_detach_null_is_safe)
{
	/* Must not crash. */
	alp_uart_rx_ringbuf_detach(NULL);
}

/* ---------- #600: exclusive ownership + parent-close cleanup -------------- */
/*
 * These run only on the CONFIG_ALP_SDK_UART_RX_RINGBUF=y +
 * CONFIG_UART_INTERRUPT_DRIVEN=y overlay (prj_uart_ringbuf.conf), where
 * port_id 0 is native_sim's real uart0 device -- attach actually
 * succeeds here (unlike the NULL/NOSUPPORT paths above), so double
 * attach, detach, reopen, and pool-reuse are all exercisable under
 * native_sim without HIL.
 */
#if defined(CONFIG_ALP_SDK_UART_RX_RINGBUF)

ZTEST(alp_peripheral, test_uart_rx_ringbuf_double_attach_rejected_busy)
{
	alp_uart_t *u = alp_uart_open(&(alp_uart_config_t){
	    .port_id   = 0,
	    .baudrate  = 115200,
	    .data_bits = 8,
	    .stop_bits = 1,
	    .parity    = ALP_UART_PARITY_NONE,
	});
	zassert_not_null(u, "alp_uart_open should succeed for port_id=0");

	static uint8_t backing_a[64];
	static uint8_t backing_b[64];

	alp_uart_rx_ringbuf_t *rb_a = alp_uart_rx_ringbuf_attach(u, backing_a, sizeof(backing_a));
	zassert_not_null(rb_a, "first attach on a real device must succeed");

	/* Second attach while the first is still live must be refused --
     * NOT silently steal the IRQ callback out from under rb_a. */
	alp_uart_rx_ringbuf_t *rb_b = alp_uart_rx_ringbuf_attach(u, backing_b, sizeof(backing_b));
	zassert_is_null(rb_b, "second attach while one is live must be refused");
	zassert_equal(alp_last_error(), ALP_ERR_BUSY);

	alp_uart_rx_ringbuf_detach(rb_a);
	alp_uart_close(u);
}

ZTEST(alp_peripheral, test_uart_rx_ringbuf_detach_then_reattach_succeeds)
{
	alp_uart_t *u = alp_uart_open(&(alp_uart_config_t){
	    .port_id   = 0,
	    .baudrate  = 115200,
	    .data_bits = 8,
	    .stop_bits = 1,
	    .parity    = ALP_UART_PARITY_NONE,
	});
	zassert_not_null(u);

	static uint8_t backing_a[64];
	static uint8_t backing_b[64];

	alp_uart_rx_ringbuf_t *rb_a = alp_uart_rx_ringbuf_attach(u, backing_a, sizeof(backing_a));
	zassert_not_null(rb_a);

	/* Explicit detach releases exclusive ownership -- a fresh attach
     * on the same port must now succeed. */
	alp_uart_rx_ringbuf_detach(rb_a);

	alp_uart_rx_ringbuf_t *rb_b = alp_uart_rx_ringbuf_attach(u, backing_b, sizeof(backing_b));
	zassert_not_null(rb_b, "attach after explicit detach must succeed");

	alp_uart_rx_ringbuf_detach(rb_b);
	alp_uart_close(u);
}

ZTEST(alp_peripheral, test_uart_close_detaches_ringbuf_without_explicit_detach)
{
	alp_uart_t *u = alp_uart_open(&(alp_uart_config_t){
	    .port_id   = 0,
	    .baudrate  = 115200,
	    .data_bits = 8,
	    .stop_bits = 1,
	    .parity    = ALP_UART_PARITY_NONE,
	});
	zassert_not_null(u);

	static uint8_t backing[64];

	alp_uart_rx_ringbuf_t *rb = alp_uart_rx_ringbuf_attach(u, backing, sizeof(backing));
	zassert_not_null(rb, "attach must succeed before exercising close cleanup");

	/* Close WITHOUT detaching first -- the parent must detach/tear
     * down the IRQ path itself so the handle can't outlive its port. */
	alp_uart_close(u);

	/* A second, caller-issued detach on the now-stale handle must be
     * a safe no-op (the pool slot was already released by close). */
	alp_uart_rx_ringbuf_detach(rb);
	zassert_equal(alp_uart_rx_ringbuf_count(rb), 0u);
}

ZTEST(alp_peripheral, test_uart_reopen_after_close_allows_fresh_attach_pool_reuse)
{
	alp_uart_t *u1 = alp_uart_open(&(alp_uart_config_t){
	    .port_id   = 0,
	    .baudrate  = 115200,
	    .data_bits = 8,
	    .stop_bits = 1,
	    .parity    = ALP_UART_PARITY_NONE,
	});
	zassert_not_null(u1);

	static uint8_t backing_1[64];
	static uint8_t backing_2[64];

	alp_uart_rx_ringbuf_t *rb1 = alp_uart_rx_ringbuf_attach(u1, backing_1, sizeof(backing_1));
	zassert_not_null(rb1);

	/* Close port A with its ring buffer still attached -- exercises
     * the same pool slot (both the UART handle pool and the ring
     * buffer pool are static arrays) being handed to a brand-new
     * logical owner afterwards. */
	alp_uart_close(u1);

	alp_uart_t *u2 = alp_uart_open(&(alp_uart_config_t){
	    .port_id   = 0,
	    .baudrate  = 115200,
	    .data_bits = 8,
	    .stop_bits = 1,
	    .parity    = ALP_UART_PARITY_NONE,
	});
	zassert_not_null(u2, "reopen after close must succeed");

	alp_uart_rx_ringbuf_t *rb2 = alp_uart_rx_ringbuf_attach(u2, backing_2, sizeof(backing_2));
	zassert_not_null(rb2,
	                 "fresh attach on the reopened port must succeed, "
	                 "not be refused by a stale back-ref surviving pool reuse");

	alp_uart_rx_ringbuf_detach(rb2);
	alp_uart_close(u2);
}

#endif /* CONFIG_ALP_SDK_UART_RX_RINGBUF */
