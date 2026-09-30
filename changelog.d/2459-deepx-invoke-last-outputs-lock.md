### Fixed — DEEPX `invoke()` no longer races on `last_outputs` (#2459)

`alp_inference_deepx_invoke()` assigned `last_outputs` under a shared lock, so two concurrent invokes, or an invoke against `alp_inference_deepx_get_output()`, raced on a vector of `shared_ptr`s. `invoke()` now takes `engine_mtx` exclusively; `get_output()` keeps the shared lock.
