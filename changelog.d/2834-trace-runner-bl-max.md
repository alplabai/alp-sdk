### Added — the trace-runner's backlight ceiling is a build option, `TR_BL_MAX_PCT`

`-DTR_BL_MAX_PCT=<5..80, a multiple of 5>` (default 80, checked at configure time) sets the hard ceiling of the encoder's BRIGHTNESS mode, the bench request word and the boot level, for carriers whose 5 V net droops under the backlight (40 for the exhibition unit). `a32/release/build-release.sh` prints the ceiling the HE was built with and, with `TR_BL_MAX_PCT=<n>` in the environment, refuses an HE built with another one; `tests/host/test_knob.c` also runs at `-DTR_BL_MAX_PCT=40`.
