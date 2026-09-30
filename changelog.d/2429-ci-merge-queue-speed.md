### Changed — `python-smoke`/`loader-smoke` skip on the `dev` merge queue instead of force-running (#2429)

`dev` merge-queue entries sat 20-30+ minutes in `AWAITING_CHECKS`, dominated
by `pr-twister`'s required `twister-shard 1/4..4/4` (13-25 minutes per shard,
already cached). `cross-platform-zephyr.yml`'s `python-smoke` and
`loader-smoke` jobs are not required contexts on `dev`, but on `merge_group`
they force-ran an ubuntu-only leg anyway (plus the `python-smoke · all`
summary), pulling 3 extra jobs per queue entry from the same shared
`ubuntu-latest` runner pool the required `twister-shard`/`distro-install`
jobs are waiting on -- exactly the runner-starvation shape
`merge-queue-ejection-alarm.yml` (#1952) exists to catch (a real ejection
hit PR #1887 on 2026-09-04).

Every commit that reaches the merge queue already ran `python-smoke` and
`loader-smoke` on `pull_request` before being enqueued, so re-running them on
`merge_group` re-validates nothing new. Both jobs' `if:` now skips
unconditionally on `merge_group`; the `python-smoke · all` summary job's
verdict script treats that skip as a pass. Coverage is unchanged -- the same
content was already graded on the PR event -- only the redundant re-run on
the queue ref is removed. The `merge_group:` trigger stays on the workflow
(dropping it would reopen #1415's AWAITING_CHECKS hang for whichever future
PR promotes `python-smoke · all` to a required context, per #1528).

Not changed here (needs a branch-protection edit, not a workflow file, so
left as a recommendation on #2429): `pr-twister`'s 4-way shard split is the
actual long pole, but its required-context names are literally
`twister-shard 1/4` .. `4/4`, so a shard-count change renames required
checks. The current merge-queue build-concurrency setting is also worth the
maintainer's review against #1952's runner-starvation precedent.
