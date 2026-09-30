### Fixed — docs no longer cite a nonexistent `dxrt_init()` (#2460)

dx_rt has no `dxrt_init()`. The bring-up docs, test plan, verification status, `deepx_dxm1.h` and the V2M DEEPX dtsi comment now name the real check: constructing `dxrt::InferenceEngine`, which opens `/dev/dxrt0` with `dxrt.service` running.
