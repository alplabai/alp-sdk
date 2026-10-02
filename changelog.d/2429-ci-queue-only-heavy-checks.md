### Changed — twister and distro-install run in the merge queue, not on PRs into `dev`

On the free plan the org gets 20 concurrent Actions jobs. A full twister run
holds 9 of them, so PR-time runs kept the merge queue's own runs waiting for
runners, and a burst of PRs stalled every merge.

- `pr-twister.yml` and `pr-bootstrap-distro-install.yml` skip their work on
  a pull_request into `dev`; the required contexts
  `twister · native_sim/native/64` and `distro install · all` report success
  at PR time. The real run is the PR's `merge_group` run, against the exact
  commit that lands. PRs into `main` still run both on pull_request.
- `pr-twister.yml` no longer runs on `push` to `dev`: that commit is the one
  its merge-queue run already tested. The nightly full run on `dev` stays the
  backstop and now carries the ccache save alone.
- `clang-format · diff-only` still runs on every PR.

Run `bash scripts/test-all.sh` before opening a PR into `dev`: a failure now
surfaces in the queue (ejecting the entry) rather than on the PR.
