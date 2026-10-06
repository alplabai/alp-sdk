### Fixed — alp-sdk Yocto recipe now inherits pkgconfig so the gles PACKAGECONFIG configures (#2678)

The `gles` PACKAGECONFIG (Mali gpu2d backend) makes CMake detect `egl.pc`/`glesv2.pc` through `pkg_check_modules`, but `alp-sdk_0.6.bb` only inherited `cmake`, so `do_configure` failed for lack of `pkg-config-native`. The recipe now reads `inherit cmake pkgconfig`.
