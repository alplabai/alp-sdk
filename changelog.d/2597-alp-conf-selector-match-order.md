### Fixed — per-SKU `alp.conf` selector sent every AEN board to a per-SKU file, written or not (#2597)

`twister · AEN801` and `twister · AEN803` have been red on `dev` since the per-SKU selector landed (#2601, 2026-10-01): eight scenarios in five examples stopped at CMake with `File not found: .../generated/aen801/alp.conf` (or `aen803`).

The selector in each example's `CMakeLists.txt` tested the regex and the file in one condition:

```cmake
if(BOARD MATCHES "^alp_e1m_(aen[0-9]+)"
   AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/generated/${CMAKE_MATCH_1}/alp.conf")
```

CMake expands `${CMAKE_MATCH_1}` before it evaluates the `if()`, so `EXISTS` saw `generated//alp.conf`, which is always there. The body then ran with `CMAKE_MATCH_1` set, and pointed `EXTRA_CONF_FILE` at `generated/<sku>/alp.conf`. That file exists only for a twin SKU whose fragment differs, so a build for the SKU the example's own `board.yaml` names failed, and so would a twin whose fragment is identical.

The five selectors now nest the two tests, so `CMAKE_MATCH_1` is set before `EXISTS` reads it: `examples/peripheral-io/blink/CMakeLists.txt:11` ("if(BOARD MATCHES "^alp_e1m_(aen[0-9]+)")"), and the same block in `alp-console`, `drone-autopilot`, `pwm-led-fade` and `connectivity/camera-mjpeg-stream`.

`tests/scripts/test_gen_example_alp_conf.py` runs each example's selector through real CMake (`cmake -P`) against a fake `generated/` tree: a board with no per-SKU directory stays on `generated/alp.conf`, a board with one gets it, a non-AEN board is untouched. The existing tests covered the generator only, which is why the selector shipped broken.
