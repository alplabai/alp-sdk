### Changed — pr-twister-aen runs only the AEN SKU a PR can affect (#2541)

`pr-twister-aen.yml` gains a `select` job that runs
`scripts/select_aen_twister.py` on pull requests and builds the matrix from
its answer. A PR whose only non-doc changes sit under one SKU's board
directory (`zephyr/boards/alp/e1m_aen801_m55_*` or `e1m_aen803_m55_*`) now
runs just that SKU's two `--subset` legs; a PR touching only changelog, docs,
Markdown or `tests/scripts/` runs none. Any shared path, a failed git call, a
push, the nightly schedule and `workflow_dispatch` still run all four legs.
The workflow stays path-filtered and is not a required context.

Not done: per-example `--testsuite-root` narrowing, which would reshuffle
the `--subset` partition and the per-shard ccache.
