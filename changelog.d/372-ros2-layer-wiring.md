### Changed — ROS 2 layer wiring built and recorded on the host (#372)

**`bitbake alp-perception` and `bitbake alp-image-edge` for `MACHINE=e1m-v2n101-a55` with `meta-ros2-humble` (`scarthgap`) exit 0 on the host, and the image rootfs manifest lists `rclcpp` and `alp-perception`.** `meta-alp-sdk/README.md` records the result; `ros2` stays Tier B because ADR 0018 Tier A needs a CI build, so Tier A waits on an internal `bitbake` CI lane.
