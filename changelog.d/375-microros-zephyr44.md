### Fixed — micro-ROS M33 slice builds on Zephyr 4.4 / zephyr-sdk 1.0.1 (#375)

`micro_ros_zephyr_module` (pinned at `8477de12`) failed in its colcon
cross-build (`unknown type name '__errno_t'`, then `strcasecmp`,
`zephyr/posix/time.h`, `isatty`) and at link (`STATIC_INIT_GNU`) against
Zephyr 4.4 with the picolibc toolchain. `microros-ros2-v2n/m33_sm/patches/0002`
fixes the module's flag derivation and Kconfig; the example now applies every
`patches/*.patch` in order, to a per-build copy of the module under the build
directory (the shared west checkout is left untouched). Upstream draft: `docs/upstream/micro-ros-zephyr-4.4.md`.
Build only; not run on hardware.
