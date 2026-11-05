### Fixed — `ALP_SDK_CHIP_CC3501E` help text pointed at the abandoned `alplabai/cc3501e-firmware` repo (#3060)

The help text said the CC3501E firmware lives in `alplabai/cc3501e-firmware`
"per ADR 0005". That repo was only ever a scaffold; ADR 0015 dropped the
plan, and the firmware lives in `alplabai/cc3501e-bridge-firmware`. The help
text now names that repo and points at `docs/cc3501e-bridge.md`.
