/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * fmt-formatting -- teaches {fmt} (https://github.com/fmtlib/fmt)'s
 * `fmt::format_to` API: formatting into a caller-owned fixed buffer,
 * with NO heap allocation and NO <iostream>.
 *
 * What success looks like:
 *
 *   [fmt-formatting] buffer: "count=7 ratio=3.14 label=sensor-a"
 *   [fmt-formatting] done
 *
 * Where this runs: native_sim.  `board.yaml`'s `libraries: [fmt]`
 * adds vendors/fmt/include (the vendored fmt 11.0.2 headers) and
 * metadata/library-profiles/fmt/fmt_config.h (FMT_HEADER_ONLY=1,
 * FMT_USE_IOSTREAM=0, FMT_EXCEPTIONS=0) to the include path. This
 * example's prj.conf sets CONFIG_REQUIRES_FULL_LIBCPP=y because
 * fmt/format.h needs <string_view>/<type_traits>/<limits> that
 * Zephyr's minimal C++ library doesn't ship -- see prj.conf.
 *
 * No allocation, no <iostream>:
 *
 *   fmt::format_to() writes formatted output directly through an
 *   output iterator the caller supplies -- here, a raw `char*` into
 *   a stack-allocated `char[64]`.  Nothing is heap-allocated, and
 *   nothing pulls in <iostream>'s ~30 KB of static-init code (that's
 *   what FMT_USE_IOSTREAM=0 in the profile buys). Compare this to
 *   fmt::format(), which returns an owning std::string -- the right
 *   choice on a host build, the wrong one on a heap-less firmware
 *   build.
 */

/* metadata/library-profiles/fmt/fmt_config.h MUST be included before
 * any <fmt/...> header -- unlike ETL (which probes for
 * "etl_profile.h" itself via #include from inside its own headers),
 * fmt has no such auto-include hook; the profile's FMT_HEADER_ONLY /
 * FMT_USE_IOSTREAM / FMT_EXCEPTIONS #defines only take effect if
 * they're visible before fmt/base.h's own `#ifndef FMT_HEADER_ONLY`
 * checks run.  Same pattern as
 * examples/testing/doctest-selftest/src/main.cpp's
 * `#include "doctest_config.h"` line. */
#include "fmt_config.h"

#include <fmt/format.h>

#include <cstdio>

int main(void)
{
	/* Stack storage, sized generously for this format string --
	 * fmt::format_to does NOT null-terminate or bounds-check on
	 * its own (unlike fmt::format_to_n, which takes an explicit
	 * size limit); the caller is responsible for both, exactly
	 * like snprintf() with a size you trust rather than measure. */
	char buf[64];

	int         count = 7;
	float       ratio = 3.14F;
	const char *label = "sensor-a";

	/* fmt::format_to returns an iterator ONE PAST the last
	 * character written -- here that's just `buf + N`, since the
	 * output iterator is a plain `char*`. We use it to know exactly
	 * where to place the NUL terminator ourselves. */
	char *end = fmt::format_to(buf, "count={} ratio={:.2f} label={}", count, ratio, label);
	*end      = '\0';

	printf("[fmt-formatting] buffer: \"%s\"\n", buf);

	printf("[fmt-formatting] done\n");
	return 0;
}
