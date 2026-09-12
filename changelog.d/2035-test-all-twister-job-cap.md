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

`ALP_TWISTER_JOBS` now caps the concurrent build jobs, passed through as
twister's `-j`. Leaving it unset keeps twister's own default, so CI is
unchanged and nobody has to set anything. Set it on a shared or
memory-tight host; roughly one job per 2 GB of RAM is a safe starting
point, and lower if anything else heavy is running alongside.
