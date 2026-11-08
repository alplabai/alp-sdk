### Changed — ROS 2 library manifest promoted to Tier A (#372)

**`metadata/libraries/ros2.yaml` is now `tier: A`: `bitbake alp-perception` and `bitbake alp-image-edge` for `MACHINE=e1m-v2n101-a55` with upstream `meta-ros2-humble` (meta-ros `scarthgap`) both built (RC=0) on the host, and the image's rootfs manifest lists `rclcpp` and `alp-perception`.** `meta-alp-sdk/README.md` and `metadata/libraries/README.md` record the build and the supported command.
