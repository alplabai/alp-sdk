/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression for alp_sigpipe_safe_write() (src/yocto/alp_sigpipe_safe_
 * write.c, issue #2389): a broken-pipe write must return a clean `false`
 * (mappable to ALP_ERR_IO by its caller) instead of raising SIGPIPE and
 * killing the process -- the exact "tar died, fwrite() into the dead
 * pipe took the app down with it" failure the DRP-AI tar-staging
 * extractor (src/yocto/inference_drpai.cpp) hit in the issue's repro
 * (process exit status 141).
 *
 * Deliberately host-portable: this helper is plain POSIX
 * (pthread_sigmask/sigpending/sigwait, no Linux-only header), unlike
 * inference_drpai.cpp itself (hard #includes <linux/drpai.h> et al., see
 * tests/yocto/inference_drpai_regression.cpp's Linux-only CMake gate) --
 * so this test builds and runs on the maintainer's macOS dev host too,
 * proving the write helper's SIGPIPE-safety without needing a real
 * `tar`, a real DRP-AI device, or a Linux CI runner. It uses a plain
 * pipe(2) with the read end closed to reproduce "the reader already
 * exited" deterministically, without any real subprocess at all.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_sigpipe_safe_write
 *   ctest --test-dir build -R alp_test_sigpipe_safe_write
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include "../../src/yocto/alp_sigpipe_safe_write.h"
#include "test_assert.h"

static void test_write_succeeds_when_reader_is_present(void)
{
	int fds[2];
	ALP_ASSERT_EQ_INT(pipe(fds), 0);

	static const char msg[] = "hello drpai staging";
	bool              ok    = alp_sigpipe_safe_write(fds[1], msg, sizeof(msg));
	close(fds[1]);

	char    buf[64] = { 0 };
	ssize_t n       = read(fds[0], buf, sizeof(buf));
	close(fds[0]);

	ALP_ASSERT_TRUE(ok);
	ALP_ASSERT_EQ_INT((int)n, (int)sizeof(msg));
}

/* The core regression: a reader that is already gone (the "tar exited on
 * a bad header" case) must make the write FAIL cleanly, not kill this
 * process.  Simply reaching the assertions below at all is half the
 * proof -- with the pre-fix plain fwrite()/write() and default SIGPIPE
 * disposition, this test process would have died at the write() call
 * with exit status 141 and never printed a result. */
static void test_write_survives_closed_reader_no_sigpipe_death(void)
{
	int fds[2];
	ALP_ASSERT_EQ_INT(pipe(fds), 0);
	close(fds[0]); /* no reader left -- every write() below hits EPIPE/SIGPIPE */

	static const char msg[] = "the process must survive this write";
	bool              ok    = alp_sigpipe_safe_write(fds[1], msg, sizeof(msg));
	close(fds[1]);

	ALP_ASSERT_TRUE(!ok);
}

/* alp_sigpipe_safe_write() must not leave a stray pending SIGPIPE behind
 * on this thread after it returns -- a caller doing unrelated work later
 * (or a signal handler installed by the embedding app) must not observe
 * a signal this helper generated and was supposed to have consumed. */
static void test_no_pending_sigpipe_leaks_after_call(void)
{
	int fds[2];
	ALP_ASSERT_EQ_INT(pipe(fds), 0);
	close(fds[0]);

	static const char msg[] = "x";
	(void)alp_sigpipe_safe_write(fds[1], msg, sizeof(msg));
	close(fds[1]);

	sigset_t pending;
	ALP_ASSERT_EQ_INT(sigpending(&pending), 0);
	ALP_ASSERT_TRUE(sigismember(&pending, SIGPIPE) == 0);
}

/* A SIGPIPE already pending BEFORE the call belongs to whoever raised
 * it, not to this call's own write -- alp_sigpipe_safe_write() must
 * leave it exactly as pending as it found it, not consume it as a side
 * effect of servicing an unrelated successful write. */
static void test_preexisting_pending_sigpipe_is_not_consumed(void)
{
	sigset_t sigpipe_set;
	sigemptyset(&sigpipe_set);
	sigaddset(&sigpipe_set, SIGPIPE);

	sigset_t old_mask;
	ALP_ASSERT_EQ_INT(pthread_sigmask(SIG_BLOCK, &sigpipe_set, &old_mask), 0);

	/* Raise SIGPIPE against this (now SIGPIPE-blocked) thread ourselves --
	 * it becomes pending rather than delivered.  Stands in for a SIGPIPE
	 * some other part of the embedding app already raised before ever
	 * calling into alp_sigpipe_safe_write(). */
	ALP_ASSERT_EQ_INT(raise(SIGPIPE), 0);

	sigset_t pending_before_call;
	ALP_ASSERT_EQ_INT(sigpending(&pending_before_call), 0);
	ALP_ASSERT_TRUE(sigismember(&pending_before_call, SIGPIPE) == 1);

	/* An ordinary write through a live pipe -- only the "is the
	 * pre-existing pending SIGPIPE left alone" behaviour is under test
	 * here, so the write itself must succeed normally. */
	int fds[2];
	ALP_ASSERT_EQ_INT(pipe(fds), 0);
	static const char msg[] = "y";
	bool              ok    = alp_sigpipe_safe_write(fds[1], msg, sizeof(msg));
	close(fds[1]);
	char buf[8] = { 0 };
	(void)read(fds[0], buf, sizeof(buf));
	close(fds[0]);
	ALP_ASSERT_TRUE(ok);

	sigset_t pending_after_call;
	ALP_ASSERT_EQ_INT(sigpending(&pending_after_call), 0);
	ALP_ASSERT_TRUE(sigismember(&pending_after_call, SIGPIPE) == 1);

	/* Clean up: consume the SIGPIPE this test raised itself (sigwait()
	 * returns an error NUMBER directly, not -1 with errno -- see
	 * alp_sigpipe_safe_write.c's own comment on this), then restore the
	 * thread's original mask so later tests are unaffected. */
	int consumed = 0;
	int r;
	do {
		r = sigwait(&sigpipe_set, &consumed);
	} while (r == EINTR);
	pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
}

/* A zero-length write (the DRP-AI extractor's `len == 0` short-circuit
 * calls this helper only for len > 0, but the helper itself must still
 * behave for len == 0 -- no write() call, no false failure). */
static void test_zero_length_write_is_a_trivial_success(void)
{
	int fds[2];
	ALP_ASSERT_EQ_INT(pipe(fds), 0);

	bool ok = alp_sigpipe_safe_write(fds[1], NULL, 0);
	close(fds[0]);
	close(fds[1]);

	ALP_ASSERT_TRUE(ok);
}

int main(void)
{
	test_write_succeeds_when_reader_is_present();
	test_write_survives_closed_reader_no_sigpipe_death();
	test_no_pending_sigpipe_leaks_after_call();
	test_preexisting_pending_sigpipe_is_not_consumed();
	test_zero_length_write_is_a_trivial_success();

	ALP_TEST_SUMMARY();
}
