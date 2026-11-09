### Fixed — `aen-evk-demo` phase 11 drives U46 `S` before `/E` (#2992)

**`examples/aen/aen-evk-demo` now sets the U46 select line `S` before asserting `/E` in phase 11, so the mux never briefly routes the wrong input.** Only the example's pin ordering changes; no API or board.yaml change.
