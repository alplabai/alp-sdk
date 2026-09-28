### Fixed

- `mera2-drpai-tvm`: `libmera_drpai_wrapper.so` now links `-lfmt` (and `DEPENDS` on `fmt`). With `SPDLOG_FMT_EXTERNAL` the wrapper calls `fmt::v10` directly but never recorded `libfmt.so` in its `DT_NEEDED`, so any image that pulled in `alp-drpai-inference` failed its `do_compile` final link with `undefined reference to fmt::v10::detail::vformat_to`.
- `dx-rt`: new `dynamic-layers/meta-deepx-m1` bbappend drops `${bindir}/*` from `FILES:${PN}`. meta-deepx-m1 appends `dx-rt-cli`/`dx-rt-examples` after `${PN}`, so `${PN}` swallowed every tool and `dx-rt-cli` was never written; `alp-image-edge` with `ALP_ENABLE_DEEPX_DXM1 = "1"` failed `do_rootfs` with `Error: Unable to find a match: dx-rt-cli`.
