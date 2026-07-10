/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * doctest-selftest -- a minimal doctest v2.4.11 unit-test binary.
 *
 * This is a HOST unit-test example, not a firmware demo: there's no
 * app logic to exercise on a SoM.  The point is to show how the
 * doctest single-header test framework is wired into an Alp SDK
 * project via `libraries: [doctest]`, and to prove the wiring by
 * building + running a small test suite that must pass on
 * native_sim.  [UNTESTED] on real silicon -- see the README.
 *
 * What success looks like:
 *
 *   [doctest] doctest version is "2.4.11"
 *   [doctest] run with "--help" for options
 *   ===============================================================================
 *   [doctest] test cases:  3 |  3 passed | 0 failed | 0 skipped
 *   [doctest] assertions:  6 |  6 passed | 0 failed |
 *   [doctest] Status: SUCCESS!
 *   [doctest-selftest] done
 *
 * doctest prints its own pass/fail summary; we print the SDK's
 * uniform `[<name>] done` marker only after confirming doctest's
 * run returned zero failures, so the twister harness has one
 * stable line to regex on regardless of doctest's summary
 * formatting across versions.
 */

/* metadata/library-profiles/doctest/doctest_config.h MUST be
 * included before <doctest/doctest.h> -- it #defines the
 * DOCTEST_CONFIG_NO_* knobs that keep the runner off POSIX signal
 * handling and multithreading, neither of which exist the same way
 * on Cortex-M as they do on the native_sim host.  The profile wins
 * over upstream's defaults per the "profile header" contract
 * documented in metadata/library-profiles/README.md. */
#include "doctest_config.h"

/* Exactly ONE translation unit defines DOCTEST_CONFIG_IMPLEMENT --
 * it makes this #include emit doctest's runner implementation (the
 * Context class, the reporters, TEST_CASE registration machinery)
 * instead of just the macro declarations.  We deliberately do NOT
 * use DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN: that variant supplies its
 * own `main()`, and we need our own so we can print the SDK's
 * `[doctest-selftest] done` marker after a passing run. */
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>

/* --------------------------------------------------------------------
 * The trivial pure function under test.
 *
 * doctest doesn't care what it's testing -- the point of this
 * example is the framework wiring, so `clamp()` is deliberately
 * boring: no headers, no state, no hardware. Real projects would
 * `#include` the header that declares the function they're testing
 * instead of defining it inline like this.
 * ------------------------------------------------------------------ */
static int clamp(int value, int lo, int hi)
{
	if (value < lo) {
		return lo;
	}
	if (value > hi) {
		return hi;
	}
	return value;
}

/* TEST_CASE registers a free function with doctest's global test
 * registry via a static-init side effect (a namespaced global
 * object whose constructor runs before main()) -- nothing calls
 * these bodies directly. CHECK() records a failure and continues
 * the test case; REQUIRE() (not used here) aborts the test case on
 * first failure. Three cases cover the three branches of clamp(). */

TEST_CASE("clamp: value inside the range passes through unchanged")
{
	CHECK(clamp(5, 0, 10) == 5);
	CHECK(clamp(0, 0, 10) == 0);
}

TEST_CASE("clamp: value below the range saturates to lo")
{
	CHECK(clamp(-3, 0, 10) == 0);
	CHECK(clamp(-1000, -5, 5) == -5);
}

TEST_CASE("clamp: value above the range saturates to hi, boundary is inclusive")
{
	CHECK(clamp(42, 0, 10) == 10);
	CHECK(clamp(10, 0, 10) == 10);
}

int main(void)
{
	/* `int main(void)`, NOT `main(int argc, char **argv)`: unless
	 * the app sets CONFIG_BOOTARGS, Zephyr's kernel init
	 * (kernel/init.c) invokes the application entry point through
	 * a bare `extern int main(void); main();` call -- no arguments
	 * are ever placed in the argc/argv registers. Declaring `main`
	 * with (argc, argv) parameters here would silently read
	 * whatever garbage happened to be left in those registers at
	 * the call site instead of a real command line (this exact
	 * mistake segfaulted catch2-selftest's first draft inside
	 * Catch2's argv-walking applyCommandLine() -- see that
	 * example's README). We skip command-line parsing entirely --
	 * there IS no host command line to parse on native_sim -- and
	 * just run every registered TEST_CASE with doctest's defaults.
	 *
	 * doctest::Context owns one test run: it runs every registered
	 * TEST_CASE and prints the summary block shown in the file
	 * header comment above. */
	doctest::Context ctx;

	int res = ctx.run();

	/* shouldExit() is true when the command line asked doctest to
	 * do something other than run tests (e.g. --help, --list-test-cases)
	 * -- in that case `res` is doctest's own exit code and there's
	 * no test result to report on. */
	if (ctx.shouldExit()) {
		return res;
	}

	/* Non-zero means at least one CHECK/REQUIRE failed -- do NOT
	 * print the success marker in that case, so a real regression
	 * shows up as a missing `[doctest-selftest] done` line rather
	 * than a false pass. */
	if (res != 0) {
		return res;
	}

	printf("[doctest-selftest] done\n");
	return 0;
}
