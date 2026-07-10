/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * catch2-selftest -- a minimal Catch2 v3 unit-test binary.
 *
 * This is a HOST unit-test example, not a firmware demo: there's no
 * app logic to exercise on a SoM. The point is to show how the
 * Catch2 "amalgamated" single-translation-unit distribution is
 * wired into an Alp SDK project via `libraries: [catch2]`, and to
 * prove the wiring by building + running a small test suite that
 * must pass on native_sim. [UNTESTED] on real silicon -- see the
 * README.
 *
 * What success looks like (Catch2's own summary line, then the
 * SDK's uniform marker):
 *
 *   ===============================================================================
 *   All tests passed (6 assertions in 3 test cases)
 *
 *   [catch2-selftest] done
 *
 * Catch2 ships as two files under Catch2's extras/ directory:
 * catch_amalgamated.hpp (all the TEST_CASE/CHECK/REQUIRE macros +
 * declarations) and catch_amalgamated.cpp (the runner
 * implementation -- Session, reporters, the discovery machinery).
 * CMakeLists.txt compiles catch_amalgamated.cpp as an extra source
 * on the `app` target, straight out of the west-fetched
 * `modules/lib/catch2` checkout -- Catch2 has no zephyr/module.yml
 * of its own, so this app's CMakeLists.txt does the wiring a
 * Zephyr module's CMakeLists.txt would normally do.
 */

#include <catch_amalgamated.hpp>

#include <cstdio>

/* --------------------------------------------------------------------
 * The trivial pure function under test.
 *
 * Catch2 doesn't care what it's testing -- the point of this
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

/* TEST_CASE registers a free function with Catch2's global test
 * registry via a static-init side effect (a namespaced global
 * object whose constructor runs before main()) -- nothing calls
 * these bodies directly. CHECK() records a failure and continues
 * the test case; REQUIRE() (not used here) throws to abort the
 * test case on first failure. Three cases cover the three branches
 * of clamp(). */

TEST_CASE("clamp: value inside the range passes through unchanged", "[clamp]")
{
	CHECK(clamp(5, 0, 10) == 5);
	CHECK(clamp(0, 0, 10) == 0);
}

TEST_CASE("clamp: value below the range saturates to lo", "[clamp]")
{
	CHECK(clamp(-3, 0, 10) == 0);
	CHECK(clamp(-1000, -5, 5) == -5);
}

TEST_CASE("clamp: value above the range saturates to hi, boundary is inclusive", "[clamp]")
{
	CHECK(clamp(42, 0, 10) == 10);
	CHECK(clamp(10, 0, 10) == 10);
}

/* CATCH_AMALGAMATED_CUSTOM_MAIN (set on the target in
 * CMakeLists.txt) tells catch_amalgamated.cpp to skip emitting its
 * own default `main()` so we can supply this one instead -- the
 * same pattern doctest-selftest uses (a custom main, not the
 * "_WITH_MAIN" convenience macro), so the example can print the
 * SDK's uniform `[catch2-selftest] done` marker only after Catch2
 * reports zero failures. */
int main(void)
{
	/* `int main(void)`, NOT `main(int argc, char *argv[])`: unless
	 * the app sets CONFIG_BOOTARGS, Zephyr's kernel init
	 * (kernel/init.c) invokes the application entry point through
	 * a bare `extern int main(void); main();` call -- no arguments
	 * are ever placed in the argc/argv registers. Declaring `main`
	 * with (argc, argv) parameters here would silently read
	 * whatever garbage happened to be left in those registers at
	 * the call site (this is exactly what crashed the first
	 * version of this example: session.applyCommandLine() walking
	 * a bogus argv). We skip command-line parsing entirely --
	 * there IS no host command line to parse on native_sim -- and
	 * just run every registered TEST_CASE with Catch2's defaults.
	 *
	 * Catch::Session owns one test run: it runs every registered
	 * TEST_CASE and prints the summary line shown in the file
	 * header comment above. */
	Catch::Session session;

	int num_failed = session.run();

	/* Non-zero means at least one CHECK/REQUIRE failed -- do NOT
	 * print the success marker in that case, so a real regression
	 * shows up as a missing `[catch2-selftest] done` line rather
	 * than a false pass. */
	if (num_failed != 0) {
		return num_failed;
	}

	printf("[catch2-selftest] done\n");
	return 0;
}
