### Added — dxrt-cli warns before DX-M1 firmware and configuration commands (#2634)

DX-M1 firmware on E1M-V2M modules is an Alp Lab specific build written at the factory; stock DEEPX firmware leaves the NPU unusable and there is no recovery in the field. `dxrt-cli -u`, `-w` and `-C` now print a warning to stderr and then run exactly as upstream (nothing is refused; the kernel driver and the library entry points, including voltage-monitor profiling, are unmodified). Built into the `e1m-v2m103-a55` image (container build); not run on a board.
