### Added — optional GPIO marker high during each `alp_inference_invoke()`

`CONFIG_ALP_INFERENCE_MARKER` drives the GPIO behind the `alp-inference-marker` devicetree alias high for the duration of every `alp_inference_invoke()`, so an external probe can window per-inference energy. Off by default, Zephyr only, and outside the latency measurement. See `docs/measuring-inference-energy.md`.
