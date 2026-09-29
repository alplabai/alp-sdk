### Changed

- CI: `pr-twister.yml` splits the native_sim twister run into 6 shards (`twister-shard 1/6`..`6/6`) instead of 4. With 4 shards the queue's long pole was one 25-minute shard, because `--subset` splits by suite count, not time. The union of the shards is still one full run, so coverage is unchanged. The required twister context on `dev` is now the aggregator `twister · native_sim/native/64` (as on `main`), so the shard count no longer appears in branch protection (#2429).
