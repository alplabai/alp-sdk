# Performance baselines

Per-target captures of the `alp_bench` microbench output, recorded
via [`tests/bench/baseline_runner.py`](../baseline_runner.py)'s `--record`
mode.  Used to gate against perf regressions (see the script's
`--diff` mode + the v1.0 `pr-bench.yml` workflow that will run it
on every PR).

## Filename convention

`<som-sku>-<os>.yaml`, e.g. `E1M-V2N101-yocto.yaml`,
`E1M-AEN701-zephyr.yaml`.  One file per `(SoM, OS)` pair the SDK
supports; the captain owns refreshing them at every release tag
once HiL hardware is provisioned.

## Re-record after an intentional change

```bash
./build/tests/bench/alp_bench | python3 tests/bench/baseline_runner.py \
    --record --som E1M-V2N101 --os yocto \
    --toolchain "gcc 11.4 / Yocto kirkstone"
```

## Verify a PR didn't regress

```bash
./build/tests/bench/alp_bench | python3 tests/bench/baseline_runner.py \
    --diff --som E1M-V2N101 --os yocto
# exit 1 on any case > 10% slower than the baseline
```

The `pr-bench.yml` workflow (v1.0 deliverable) wraps this against
`main`'s baseline.
