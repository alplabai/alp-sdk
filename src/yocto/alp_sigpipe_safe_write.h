/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * INTERNAL -- a POSIX write() wrapper that survives a broken-pipe reader.
 *
 * alp-sdk#2389: writing into a pipe whose reader has already exited (e.g.
 * `tar` dying on a bad header) raises SIGPIPE in the writer.  SIGPIPE's
 * default disposition is process-terminating, so a plain fwrite()/write()
 * into that pipe kills the WHOLE calling application, not just the failed
 * open() call -- observed as exit status 141 on the DRP-AI `tar -xf -`
 * extractor (src/yocto/inference_drpai.cpp).
 *
 * A library must never change PROCESS-WIDE signal disposition (no
 * `signal(SIGPIPE, SIG_IGN)` here): that would silently mask a legitimate
 * SIGPIPE the embedding application relies on elsewhere (its own pipes,
 * its own broken-socket writes).  Instead this blocks SIGPIPE in the
 * CALLING THREAD ONLY, for the duration of the write loop
 * (pthread_sigmask), so a broken pipe surfaces as a normal EPIPE write()
 * error instead of a signal -- then restores the thread's original mask
 * before returning, having consumed any SIGPIPE the write actually
 * generated so it doesn't sit pending against this thread afterward.
 *
 * Portability: uses `sigpending()` + `sigwait()` to consume the pending
 * signal, NOT `sigtimedwait()` -- `sigtimedwait()` is a POSIX.1b
 * real-time-signals extension that Linux/glibc has but macOS/Darwin does
 * not, and this header intentionally has no Linux-only #include so its
 * regression test (tests/yocto/sigpipe_safe_write.c) builds and runs on
 * the maintainer's macOS dev host, not only in CI's Linux yocto leg.
 * `sigwait()` blocks until a signal in the given set is pending, but
 * `sigpending()` already established that SIGPIPE IS pending before
 * calling it, so the wait returns immediately -- same effect as a
 * zero-timeout `sigtimedwait()`, using only signal calls both platforms
 * implement.
 *
 * Not exported: no public alp header caller needs this -- it exists purely
 * as the extraction seam DRP-AI's tar-staging pipe writes through, kept
 * in its own TU so a plain-CMake POSIX host (including macOS, which
 * cannot build the rest of inference_drpai.cpp at all -- see that file's
 * hard <linux/drpai.h> include) can still compile and exercise the write
 * helper's SIGPIPE-safety directly.
 */

#ifndef ALP_SDK_SRC_YOCTO_ALP_SIGPIPE_SAFE_WRITE_H_
#define ALP_SDK_SRC_YOCTO_ALP_SIGPIPE_SAFE_WRITE_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Write @p len bytes from @p data to @p fd without SIGPIPE's
 *        default (process-killing) disposition ever firing on this
 *        thread.
 *
 * Blocks SIGPIPE in the calling thread for the duration of the write
 * loop, retries on EINTR, and maps EPIPE (or any other write() error) to
 * a clean `false` return instead of a signal.  Restores the thread's
 * original signal mask before returning, consuming a SIGPIPE generated
 * by this call's own write()s so none is left pending against the
 * thread -- but only if SIGPIPE was not ALREADY pending before this call
 * (that pending signal belongs to whatever raised it and is not this
 * function's to consume).
 *
 * @param[in] fd    Open, writable file descriptor -- e.g. one end of a
 *                   pipe to a child process' stdin.
 * @param[in] data  Bytes to write.  May be NULL when @p len is 0.
 * @param[in] len   Byte count to write.
 * @return true once all @p len bytes are written; false on any write()
 *         error, including EPIPE (the reader closed/exited early -- a
 *         corrupt or truncated consumer, not a crash), or if this
 *         thread's signal mask could not be adjusted.
 */
bool alp_sigpipe_safe_write(int fd, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ALP_SDK_SRC_YOCTO_ALP_SIGPIPE_SAFE_WRITE_H_ */
