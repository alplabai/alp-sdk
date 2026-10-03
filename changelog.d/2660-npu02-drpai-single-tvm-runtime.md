### Fixed — the Yocto DRP-AI backend no longer links meta-rz-drpai's `libtvm_runtime` (audit NPU-02) (#2660)

`src/yocto/CMakeLists.txt` linked `libtvm_runtime.so.2.5.1` from meta-rz-drpai's
`lib-tvm` in addition to the RUHMI 2.7.0 libraries, so a process that used the
DRP-AI backend loaded two TVM runtimes. RUHMI's own V2N apps link only
`mera2_runtime`, `mera2_plan_io`, `drp_tvm_rt` and `pthread`, and
`libdrp_tvm_rt.so` already carries the `tvm::runtime` symbols it needs, so
`TVM_RUNTIME_LIB` is gone from the link line and its `find_library`, and
`PACKAGECONFIG[drpai]` in the alp-sdk recipe no longer DEPENDS on `lib-tvm`
(`drpai` still supplies `<linux/drpai.h>`). Not yet built or run against a
real RUHMI checkout: confirm with `readelf -d libalp_sdk.so` and
`LD_DEBUG=bindings` during one inference. The image-level `lib-tvm` install in
`alp-image-common.inc` is unchanged pending a maintainer decision.
