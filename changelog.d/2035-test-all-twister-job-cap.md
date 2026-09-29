### Fixed — `scripts/test-all.sh` can no longer OOM the machine it runs on (#2035)

The twister stage passed no job limit, so it took twister's default of one
build job per core — and each build runs its own parallel ninja underneath,
so the real concurrent-compiler count sits well above the core count.

Measured on the 20-core, 31 GB bench gateway: a default-parallelism local
run exhausted memory and the kernel began reaping compilers,
`Out of memory: Killed process ... (cc1plus) ... anon-rss:425060kB`,
repeatedly, until the machine had to be rebooted. On that particular host
that is worse than a lost gate run, because the attached board farm goes
down with it.

`ALP_TWISTER_JOBS` now caps the concurrent test instances, passed through
as twister's `-j`. Leaving it unset keeps twister's own default, so CI is
unchanged and nobody has to set anything.

**Capping twister alone is not enough, and believing it was cost a second
near-miss.** `-j` bounds concurrent *test instances*; each instance then
runs its own ninja, which defaults to the full core count. The real
concurrent-compiler count is the product. Measured on the same 20-core
host with `ALP_TWISTER_JOBS=4`: 78 live `cc1plus` processes, 27 of 31 GB
consumed, load average 152 — the run had to be killed to avoid repeating
the very reboot the cap was added to prevent.

So when `ALP_TWISTER_JOBS` is set, the stage also exports
`CMAKE_BUILD_PARALLEL_LEVEL`, defaulting it to 2, which is what bounds the
inner build. A caller can still override it deliberately. The stage prints
both numbers and their product, so the actual ceiling is visible rather
than inferred.
