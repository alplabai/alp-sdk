### Fixed

- `mera2-drpai-tvm`: `libmera_drpai_wrapper.so` now links `-lfmt` (and `DEPENDS` on `fmt`). With `SPDLOG_FMT_EXTERNAL` the wrapper calls `fmt::v10` directly but never recorded `libfmt.so` in its `DT_NEEDED`, so any image that pulled in `alp-drpai-inference` failed its `do_compile` final link with `undefined reference to fmt::v10::detail::vformat_to`.
