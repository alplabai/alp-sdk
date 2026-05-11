/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * fmt configuration header for the ALP SDK's embedded targets.
 *
 * fmt (https://github.com/fmtlib/fmt) reads compile-time defines
 * before its core headers.  Consumers using fmt under alp.yaml's
 * libraries: list get this header force-included via the loader,
 * so they don't have to remember which knobs to set for
 * Cortex-M / Cortex-A embedded builds.
 *
 * Invariants we enforce:
 *
 *   - FMT_HEADER_ONLY      : avoid the precompiled-format-string
 *                            library; everything inlines.  Trades
 *                            link time for build-time but eliminates
 *                            a static-init concern on M-class.
 *   - FMT_USE_IOSTREAM=0   : the SDK doesn't pull in <iostream> --
 *                            it adds ~30 KB of static-init code
 *                            for nothing on embedded.
 *   - FMT_EXCEPTIONS=0     : the SDK is exception-free on the
 *                            hot path.  fmt routes invalid-format
 *                            failures through fmt::detail::throw_format_error,
 *                            which becomes a hard fault when
 *                            FMT_EXCEPTIONS=0.
 *   - FMT_USE_DOUBLE=0     : optional -- shrinks binary size if
 *                            the app never formats floating-point.
 *                            Override in your app's fmt config if
 *                            you need %f / fmt::format("{:.3f}", x).
 *
 * Consumers wanting different fmt settings supply their own
 * fmt config header; the loader prefers the app's over this one.
 */

#ifndef ALP_FMT_CONFIG_H_
#define ALP_FMT_CONFIG_H_

/* Header-only mode.  Apps that prefer the compiled library swap
 * this to 0 in their own config. */
#define FMT_HEADER_ONLY 1

/* No iostream integration -- saves ~30 KB on M-class. */
#define FMT_USE_IOSTREAM 0

/* No exceptions on the hot path.  Format-string errors fault
 * loudly via fmt::detail::throw_format_error -> assert. */
#define FMT_EXCEPTIONS 0

/* Floating-point formatting stays on by default; turn off in
 * your app's config if you need to save binary size and don't
 * format floats.  This config keeps the default reasonable
 * (apps that *do* format floats just work). */
/* #define FMT_USE_DOUBLE         0 */

#endif /* ALP_FMT_CONFIG_H_ */
