/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * See alp_sigpipe_safe_write.h for the full design rationale
 * (alp-sdk#2389).
 */

#include "alp_sigpipe_safe_write.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

bool alp_sigpipe_safe_write(int fd, const void *data, size_t len)
{
	sigset_t sigpipe_set;
	sigemptyset(&sigpipe_set);
	sigaddset(&sigpipe_set, SIGPIPE);

	/* Was SIGPIPE already pending against this thread before we touch
	 * anything?  If so, it belongs to whoever raised it -- don't consume
	 * it below, and don't attribute it to this call. */
	sigset_t pending_before;
	sigpending(&pending_before);
	const bool already_pending = sigismember(&pending_before, SIGPIPE) == 1;

	sigset_t old_mask;
	if (pthread_sigmask(SIG_BLOCK, &sigpipe_set, &old_mask) != 0) {
		/* Couldn't block SIGPIPE -- do not attempt an unsafe write. */
		return false;
	}

	const unsigned char *p     = (const unsigned char *)data;
	size_t               total = 0;
	bool                 ok    = true;
	while (total < len) {
		ssize_t n = write(fd, p + total, len - total);
		if (n < 0) {
			if (errno == EINTR) {
				continue; /* interrupted before any byte was written; retry */
			}
			ok = false; /* EPIPE (reader gone) or any other write() error */
			break;
		}
		if (n == 0) {
			ok = false; /* no forward progress; treat as an error, not a spin */
			break;
		}
		total += (size_t)n;
	}

	if (!already_pending) {
		/* Consume the SIGPIPE this call's own write()s may have raised
		 * (blocked above, so it is pending rather than delivered) --
		 * sigwait() blocks until a signal in @sigpipe_set is pending,
		 * but sigpending() already confirmed that state, so this
		 * returns immediately when it fires and is a no-op otherwise.
		 * (See the header comment for why this replaces a zero-timeout
		 * sigtimedwait(): sigtimedwait() doesn't exist on macOS.) */
		sigset_t pending_after;
		sigpending(&pending_after);
		if (sigismember(&pending_after, SIGPIPE) == 1) {
			int consumed = 0;
			while (sigwait(&sigpipe_set, &consumed) != 0 && errno == EINTR) {
				/* retry on spurious EINTR from sigwait() itself */
			}
		}
	}

	pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
	return ok;
}
