### Added — an original tilt-steered arcade game example on the RK055HDMIPI4MA0 panel (#2199)

`examples/aen/aen-tilt-arcade` ("Riftrunner") is a playable, original arcade
game for the E1M-AEN801/AEN803 on the E1M-EVK's RK055HDMIPI4MA0 720x1280
portrait panel, riding on the panel bring-up this issue tracks. A
tilt-steered craft dodges falling hazards and collects pickups; rendering
goes entirely through the portable `<alp/display.h>` surface
(`alp_display_open` / `alp_display_get_caps` / `alp_display_blit` /
`alp_display_clear`) using small dirty-rect blits rather than a per-frame
full 1,843,200-byte redraw, and steering reads the on-module BMI323 over
portable I2C (`<alp/chips/bmi323.h>`, `<alp/peripheral.h>`), falling back to
a deterministic self-steering attract mode when the IMU is absent or fails
to init. Like `aen-dsi-display`, it prints a pointer to this issue and exits
cleanly (no retry loop) if `alp_display_open()` returns `NULL` on one of the
panel's intermittent cold-boot init failures.
