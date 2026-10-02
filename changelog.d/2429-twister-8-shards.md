### Changed — `pr-twister` splits native_sim twister into 8 shards (#2429)

The merge-queue long pole is `twister-shard`. Measured on `dev` merge_group
runs 36766415005 and 36772820214 (2026-09-30, already 6 shards): longest
shard 13.7 min (of which ~2.5 min is per-shard setup: apt, west cache,
ccache restore, pip), shards 4/5/6 at 13.4-13.7 min vs 1/2/3 at 8-11 min.
Before that change (4 shards, 2026-09-29, run 36546876623): 23 min.
Moving `--subset i/6` to `i/8` shrinks each shard's test slice by a quarter
with the same union of suites. The required context is the aggregator
`twister · native_sim/native/64`, whose name is unchanged, so no
branch-protection edit is needed.
