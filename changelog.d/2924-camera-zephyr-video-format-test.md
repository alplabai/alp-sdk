### Tests — camera: host test that zephyr_video open() refuses formats it cannot negotiate (#2924)

The refusal itself (ALP_ERR_NOSUPPORT for MONO_VLSB, YUV420_PLANAR and NV12)
landed in #2998; tests/unit/camera_zephyr_video_formats now pins it on
native_sim.
