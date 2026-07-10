/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plain-CMake tests for the Yocto/Linux termios backend
 * (src/yocto/peripheral_uart.c).
 *
 * Failure-path coverage only -- real-adapter testing wants a
 * loopback (RX/TX shorted) and lives behind the v0.4 hil-yocto
 * runner.
 *
 * #595 regression coverage (bounded read timeout): alp_uart_open's
 * port_id only resolves to /dev/ttyS<N>-style paths, so there is no
 * CI-controllable route to a live external port through the public
 * API alone.  alp_uart_read_fd_bounded() (non-static, internal-only,
 * declared extern below) is the exact bounded poll()+read() loop
 * alp_uart_read() runs -- these tests drive it directly against a
 * hermetic socketpair(), which polls identically to a real tty fd.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_uart
 *   ctest --test-dir build -R alp_test_peripheral_uart
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "alp/peripheral.h"

#include "test_assert.h"

/* /dev/ttyS999 will not exist on any sane CI runner. */
#define ALP_TEST_PORT_NONEXISTENT 999u

/* Internal seam -- see src/yocto/peripheral_uart.c.  Not declared in
 * any public header (not part of the public alp/ headers); this file
 * pulls the prototype in directly, same pattern
 * tests/unit/uart_registry/src/test_uart_registry.c uses for the
 * backend-registry section symbols. */
extern alp_status_t
alp_uart_read_fd_bounded(int fd, uint8_t *data, size_t len, uint32_t timeout_ms);

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void test_null_cfg_returns_null_and_stamps_invalid(void)
{
	alp_uart_t *p = alp_uart_open(NULL);
	ALP_ASSERT_NULL(p);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_invalid_data_bits_returns_null_and_stamps_invalid(void)
{
	alp_uart_config_t cfg = {
		.port_id   = 0,
		.baudrate  = 115200,
		.data_bits = 9, /* termios only accepts 5..8 */
		.stop_bits = 1,
		.parity    = ALP_UART_PARITY_NONE,
	};
	alp_uart_t *p = alp_uart_open(&cfg);
	ALP_ASSERT_NULL(p);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_invalid_stop_bits_returns_null_and_stamps_invalid(void)
{
	alp_uart_config_t cfg = {
		.port_id   = 0,
		.baudrate  = 115200,
		.data_bits = 8,
		.stop_bits = 3, /* only 1 or 2 */
		.parity    = ALP_UART_PARITY_NONE,
	};
	alp_uart_t *p = alp_uart_open(&cfg);
	ALP_ASSERT_NULL(p);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_unsupported_baud_returns_null_and_stamps_invalid(void)
{
	alp_uart_config_t cfg = {
		.port_id   = 0,
		.baudrate  = 12345u, /* not in the termios constants table */
		.data_bits = 8,
		.stop_bits = 1,
		.parity    = ALP_UART_PARITY_NONE,
	};
	alp_uart_t *p = alp_uart_open(&cfg);
	ALP_ASSERT_NULL(p);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_nonexistent_port_returns_null_and_stamps_not_ready(void)
{
	alp_uart_config_t cfg = {
		.port_id   = ALP_TEST_PORT_NONEXISTENT,
		.baudrate  = 115200,
		.data_bits = 8,
		.stop_bits = 1,
		.parity    = ALP_UART_PARITY_NONE,
	};
	alp_uart_t *p = alp_uart_open(&cfg);
	ALP_ASSERT_NULL(p);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_NOT_READY);
}

static void test_write_on_null_port_returns_invalid(void)
{
	uint8_t      buf[1] = { 0x55 };
	alp_status_t rc     = alp_uart_write(NULL, buf, sizeof(buf));
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_read_on_null_port_returns_invalid(void)
{
	uint8_t      buf[1] = { 0 };
	alp_status_t rc     = alp_uart_read(NULL, buf, sizeof(buf), 100u);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_close_null_is_safe(void)
{
	alp_uart_close(NULL);
	ALP_TEST_PASS();
}

/* ---------- #595: bounded read timeout regression coverage ---------- */
/*
 * Every test below opens a fresh socketpair() -- poll(POLLIN)/read()
 * on a AF_UNIX SOCK_STREAM fd behaves the same as on a tty fd for the
 * purposes of alp_uart_read_fd_bounded(), and needs no /dev/tty*
 * device to exist on the CI host.
 */

static void test_read_fd_no_data_times_out_instead_of_blocking_forever(void)
{
	int sv[2];
	ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

	uint8_t buf[4] = { 0 };
	int64_t t0     = now_ms();
	/* Before the #595 fix, VMIN=1/VTIME=t never even starts its timer
     * with zero bytes ever arriving -- this call would hang forever. */
	alp_status_t rc      = alp_uart_read_fd_bounded(sv[0], buf, sizeof(buf), 50u);
	int64_t      elapsed = now_ms() - t0;

	ALP_ASSERT_EQ_INT(rc, ALP_ERR_TIMEOUT);
	/* Generous upper bound: proves "bounded", not "instant". A
     * regression back to the old blocking behaviour would hang this
     * test indefinitely rather than merely fail the assertion. */
	ALP_ASSERT_TRUE(elapsed < 2000);

	close(sv[0]);
	close(sv[1]);
}

static void test_read_fd_zero_timeout_is_a_single_nonblocking_poll(void)
{
	int sv[2];
	ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

	uint8_t      buf[4]  = { 0 };
	int64_t      t0      = now_ms();
	alp_status_t rc      = alp_uart_read_fd_bounded(sv[0], buf, sizeof(buf), 0u);
	int64_t      elapsed = now_ms() - t0;

	/* The old code treated timeout_ms==0 as VMIN=1/VTIME=0 -- pure
     * blocking, no timeout at all.  Zero must mean "poll once". */
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_TIMEOUT);
	ALP_ASSERT_TRUE(elapsed < 200);

	close(sv[0]);
	close(sv[1]);
}

static void test_read_fd_returns_ok_when_all_bytes_already_available(void)
{
	int sv[2];
	ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

	const uint8_t tx[3] = { 0x11, 0x22, 0x33 };
	ALP_ASSERT_EQ_INT(write(sv[1], tx, sizeof(tx)), (ssize_t)sizeof(tx));

	uint8_t      rx[3] = { 0 };
	alp_status_t rc    = alp_uart_read_fd_bounded(sv[0], rx, sizeof(rx), 1000u);

	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_TRUE(memcmp(rx, tx, sizeof(tx)) == 0);

	close(sv[0]);
	close(sv[1]);
}

static void test_read_fd_partial_arrival_returns_ok_with_what_arrived(void)
{
	int sv[2];
	ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

	const uint8_t tx[2] = { 0xAA, 0xBB };
	ALP_ASSERT_EQ_INT(write(sv[1], tx, sizeof(tx)), (ssize_t)sizeof(tx));

	/* Ask for 4, only 2 ever arrive -- must time out with the partial
     * bytes delivered rather than block waiting for the rest. */
	uint8_t      rx[4] = { 0 };
	alp_status_t rc    = alp_uart_read_fd_bounded(sv[0], rx, sizeof(rx), 60u);

	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_TRUE(memcmp(rx, tx, sizeof(tx)) == 0);

	close(sv[0]);
	close(sv[1]);
}

struct delayed_writer_args {
	int      fd;
	uint32_t delay_ms;
	uint8_t  byte;
};

static void *delayed_writer(void *arg)
{
	struct delayed_writer_args *a   = arg;
	struct timespec             req = {
		.tv_sec  = a->delay_ms / 1000u,
		.tv_nsec = (long)(a->delay_ms % 1000u) * 1000000L,
	};
	nanosleep(&req, NULL);
	(void)write(a->fd, &a->byte, 1); /* peer may already be gone; SIGPIPE ignored globally */
	return NULL;
}

static void test_read_fd_deadline_covers_whole_call_not_just_inter_byte_gap(void)
{
	/* The exact #595 bug shape: VMIN=1/VTIME=t restarts its timer on
     * every byte, so a byte trickling in just before one gap timer
     * expires resets the clock and the call can run far longer than
     * timeout_ms -- a slow-but-not-silent peer could stall the caller
     * almost as badly as a completely silent one.  One byte is ready
     * immediately; the second is delivered only long after the
     * requested deadline.  The call must still return at ~timeout_ms
     * with the one byte it got, not wait for the second. */
	int sv[2];
	ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

	const uint8_t first = 0x7E;
	ALP_ASSERT_EQ_INT(write(sv[1], &first, 1), 1);

	struct delayed_writer_args args = { .fd = sv[1], .delay_ms = 300u, .byte = 0x7F };
	pthread_t                  tid;
	ALP_ASSERT_EQ_INT(pthread_create(&tid, NULL, delayed_writer, &args), 0);

	uint8_t      rx[2]   = { 0 };
	int64_t      t0      = now_ms();
	alp_status_t rc      = alp_uart_read_fd_bounded(sv[0], rx, sizeof(rx), 120u);
	int64_t      elapsed = now_ms() - t0;

	ALP_ASSERT_EQ_INT(rc, ALP_OK); /* one byte collected before the deadline */
	ALP_ASSERT_EQ_INT(rx[0], first);
	/* Bounded near the requested 120 ms, nowhere near the 300 ms it
     * would take for the old per-byte-timer scheme to give up. */
	ALP_ASSERT_TRUE(elapsed >= 90 && elapsed < 250);

	pthread_join(tid, NULL); /* drains the delayed write before we close */
	close(sv[0]);
	close(sv[1]);
}

static void alarm_noop_handler(int signo)
{
	(void)signo;
}

static void test_read_fd_eintr_does_not_reset_or_extend_the_deadline(void)
{
	/* Install a SIGALRM handler WITHOUT SA_RESTART so poll() is
     * interrupted (EINTR) instead of transparently resumed, and fire
     * it repeatedly during the wait via a repeating itimer. */
	struct sigaction sa = { .sa_handler = alarm_noop_handler, .sa_flags = 0 };
	sigemptyset(&sa.sa_mask);
	struct sigaction old_sa;
	ALP_ASSERT_EQ_INT(sigaction(SIGALRM, &sa, &old_sa), 0);

	struct itimerval it = {
		.it_value    = { .tv_sec = 0, .tv_usec = 15000 },
		.it_interval = { .tv_sec = 0, .tv_usec = 15000 },
	};
	struct itimerval old_it;
	ALP_ASSERT_EQ_INT(setitimer(ITIMER_REAL, &it, &old_it), 0);

	int sv[2];
	ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

	uint8_t      buf[1]  = { 0 };
	int64_t      t0      = now_ms();
	alp_status_t rc      = alp_uart_read_fd_bounded(sv[0], buf, sizeof(buf), 100u);
	int64_t      elapsed = now_ms() - t0;

	/* Stop the timer before asserting so a failing assert doesn't
     * leave SIGALRM firing into the rest of the test binary. */
	setitimer(ITIMER_REAL, &old_it, NULL);
	sigaction(SIGALRM, &old_sa, NULL);

	ALP_ASSERT_EQ_INT(rc, ALP_ERR_TIMEOUT);
	/* Repeated EINTR must not reset the deadline (hang far past
     * 100 ms) nor shrink it (return long before 100 ms). */
	ALP_ASSERT_TRUE(elapsed >= 70 && elapsed < 400);

	close(sv[0]);
	close(sv[1]);
}

int main(void)
{
	/* The delayed-writer test below may write into sv[1] after this
     * process has already moved past the point where a peer would
     * normally still be reading -- ignore SIGPIPE globally so a race
     * never kills the whole test binary. */
	signal(SIGPIPE, SIG_IGN);

	test_null_cfg_returns_null_and_stamps_invalid();
	test_invalid_data_bits_returns_null_and_stamps_invalid();
	test_invalid_stop_bits_returns_null_and_stamps_invalid();
	test_unsupported_baud_returns_null_and_stamps_invalid();
	test_nonexistent_port_returns_null_and_stamps_not_ready();
	test_write_on_null_port_returns_invalid();
	test_read_on_null_port_returns_invalid();
	test_close_null_is_safe();

	test_read_fd_no_data_times_out_instead_of_blocking_forever();
	test_read_fd_zero_timeout_is_a_single_nonblocking_poll();
	test_read_fd_returns_ok_when_all_bytes_already_available();
	test_read_fd_partial_arrival_returns_ok_with_what_arrived();
	test_read_fd_deadline_covers_whole_call_not_just_inter_byte_gap();
	test_read_fd_eintr_does_not_reset_or_extend_the_deadline();

	ALP_TEST_SUMMARY();
}
