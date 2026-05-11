# bench/ -- ALP SDK microbenchmarks

Scaffolding for the v1.0 performance baseline.  Each public API
class in `<alp/...>` gets a small microbenchmark suite that prints
`ns/iter` numbers per case.  v1.0 will track these in CI so
regressions stop landing silently; v0.3 ships the harness + a few
representative cases.

## Design

- **Zero external deps.**  The harness lives in `bench.h` and uses
  `clock_gettime(CLOCK_MONOTONIC)` on POSIX (Yocto path) or
  `k_uptime_ticks()` on Zephyr (when wired -- v0.4).  No
  Google Benchmark, no Criterion, no bencher -- those pull in C++
  STL or Rust toolchains the SDK doesn't otherwise need.
- **One file per public API.**  `bench_peripheral.c`,
  `bench_inference.c`, `bench_status.c` today; the rest fill in
  with v1.0 as the implementations land.  Each file exposes a
  `bench_<api>_main(void)` entry that the top-level `bench_main.c`
  calls (so the bench binary stays a single executable that prints
  all cases in one run).
- **Both consumer paths benched.**  Same source builds on Zephyr +
  Yocto + baremetal; `bench.h` papers over the timing-source delta.
  Standalone-firmware authors can lift bench cases verbatim into
  their own profiling code.

## What's representative for v0.3

The SDK's public paths today are mostly NOSUPPORT stubs on
non-Zephyr backends.  That makes them *useful* bench targets --
the "fast path" through `alp_*_open(NULL)` returning NULL is the
exact cost a calling block pays when it probes for support.  Real
HW paths (open + transfer + close cycles) will dominate the file
inventory at v1.0; today's three files cover the surface-validation
half of the surface.

| File                  | What it measures                                                              |
|-----------------------|-------------------------------------------------------------------------------|
| `bench_peripheral.c`  | `alp_{i2c,spi,uart,gpio}_open` NULL/bad-arg rejection.  No HW dependency.     |
| `bench_inference.c`   | `alp_inference_open` with bad cfg; NULL-handle accessor cost.                 |
| `bench_status.c`      | `alp_last_error()` read.                                                      |
| `bench_iot.c`         | `alp_wifi_open()` + `alp_mqtt_open` NULL/empty-cfg rejection.                 |
| `bench_audio.c`       | `alp_audio_in_open` + `alp_audio_out_open` NULL/empty-cfg rejection.          |
| `bench_storage.c`     | `alp_storage_*` NULL-handle / empty-info round-trip.                          |

## Build

Opt-in via the top-level CMake option `ALP_BUILD_BENCH=ON`:

```bash
cmake -B build -DALP_OS=yocto -DALP_BUILD_BENCH=ON
cmake --build build --target alp_bench
./build/bench/alp_bench
```

The binary prints one line per case:

```
alp_i2c_open(NULL)                       1000000 iters       42 ns/iter
alp_inference_open(NULL cfg)             1000000 iters       38 ns/iter
alp_last_error                           1000000 iters        2 ns/iter
```

## Adding new bench cases

1. Create `bench/bench_<api>.c` with one `void bench_<api>_main(void)`
   entry that runs a handful of `BENCH_RUN` invocations.
2. Add the source to `bench/CMakeLists.txt`'s `target_sources`.
3. Forward-declare + call `bench_<api>_main()` from `bench_main.c`.

Each case picks an iteration count that runs in ~50-500 ms wall
clock so the entire suite finishes inside a CI minute.  Don't
over-fit -- microsecond-precision is enough for the regression
gate; nanosecond noise is fine.

## CI gate (v1.0)

v1.0 wires a `pr-bench.yml` workflow that runs `alp_bench` against
the PR's HEAD and `main`'s latest, then flags >10 % per-case
regressions.  Today the bench/ directory is opt-in only -- no CI
runs it.

## See also

- [`fuzz/`](../fuzz/) -- the libFuzzer harness suite, the other
  half of the v1.0 hardening prep (task #15).
- [`VERSIONS.md`](../VERSIONS.md) -- v1.0 milestone deliverables,
  including "Performance baselines per chip in metadata."
