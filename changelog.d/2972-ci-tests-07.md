### Fixed — ci: make the alp-build, metadata and static-analysis gates actually gate (#2972, #2973, #2974, #2975, #2996)

- pr-alp-build: matrix legs pointed at examples/rpmsg-<som>/, which moved to
  examples/multicore/rpmsg-*, so the heterogeneous legs auto-skipped green.
  Use an explicit include matrix, fail on a missing example instead of
  skipping, and rewrite som.sku to matrix.som in a CI-only board.yaml copy so
  every leg builds its own SoM (E1M-AEN701 is now exercised).
- pr-metadata-validate: path filter now covers include/alp/ext/** and
  include/alp/*.h so header-only PRs run check_vendor_ext_tags and
  check_public_header_purity; check_test_coverage runs with --fail-on-gaps
  (ratchet, currently within budget).
- pr-static-analysis: cppcheck fails the job on severity=error findings;
  stale required-context comment corrected. `.github/cppcheck/zephyr.cfg`
  (`--library`) defines the Zephyr Kconfig/devicetree `#if` macros so the 28
  src/backends and src/zephyr files cppcheck used to skip are now analysed
  instead of being reported as syntax errors. The cfg defines those macros as
  0, so cppcheck checks the inactive side only; code inside a DT/Kconfig
  `#if` branch is still not analysed.
- coverity: pass COVERITY_TOKEN through env instead of splicing it into the
  shell source.
- cross-platform-zephyr: merge_group skip message no longer claims the queue
  content was graded; docs list the contexts branch protection enforces.
